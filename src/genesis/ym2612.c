/*
 * YM2612 (OPN2) FM synthesis chip emulator.
 *
 * Implements the full 6-channel, 4-operator FM synthesis engine with
 * 8 algorithms, ADSR envelope generators, stereo panning, and DAC mode.
 *
 * Native output rate: MASTER_CLOCK / 7 / 144 ≈ 53,267 Hz (NTSC).
 * Samples are generated at the native rate and downsampled to the
 * host audio rate using a fractional accumulator.
 */

#include "ym2612.h"
#include "z80.h"
#include <string.h>
#include <math.h>
#include <stdio.h>

#define YM_CHANNELS 6
#define YM_OPERATORS 4
#define YM_NATIVE_RATE 53267

/* ------------------------------------------------------------------ */
/*  Lookup tables                                                      */
/* ------------------------------------------------------------------ */

#define SINE_TABLE_SIZE 1024
#define ENV_QUIET 0x3FF

static uint16_t sin_table[SINE_TABLE_SIZE];
static uint16_t pow_table[256];

/* Detune table from YM2612 hardware analysis (MAME / Genesis Plus GX).
 * Indexed by [detune_value 0-3][keycode 0-31]. */
static const int32_t dt_tab[4][32] = {
    /* DT = 0: no detune */
    { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
      0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
    /* DT = 1 */
    { 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2,
      2, 3, 3, 3, 4, 4, 4, 5, 5, 6, 6, 7, 8, 8, 8, 8 },
    /* DT = 2 */
    { 1, 1, 1, 1, 2, 2, 2, 2, 2, 3, 3, 3, 4, 4, 4, 5,
      5, 6, 6, 7, 8, 8, 9,10,11,12,13,14,16,16,16,16 },
    /* DT = 3 */
    { 2, 2, 2, 2, 2, 3, 3, 3, 4, 4, 4, 5, 5, 6, 6, 7,
      8, 8, 9,10,11,12,13,14,16,17,19,20,22,22,22,22 },
};

/* EG increment patterns: for each rate group (rate & 3), 8-phase cycle.
 * Determines how many attenuation steps per EG tick. */
static const uint8_t eg_inc[4][8] = {
    { 0, 1, 0, 1, 0, 1, 0, 1 },
    { 0, 1, 0, 1, 1, 1, 0, 1 },
    { 0, 1, 1, 1, 0, 1, 1, 1 },
    { 0, 1, 1, 1, 1, 1, 1, 1 },
};

static void build_tables(void)
{
    for (int i = 0; i < SINE_TABLE_SIZE; i++) {
        double s = sin(2.0 * M_PI * i / SINE_TABLE_SIZE);
        if (s == 0.0) {
            sin_table[i] = ENV_QUIET;
        } else {
            double logval = -log2(fabs(s)) * 256.0;
            if (logval > ENV_QUIET) logval = ENV_QUIET;
            sin_table[i] = (uint16_t)logval;
        }
    }
    for (int i = 0; i < 256; i++) {
        double p = pow(2.0, 1.0 - i / 256.0) * 4096.0;
        pow_table[i] = (uint16_t)(p + 0.5);
    }
}

/* ------------------------------------------------------------------ */
/*  Operator and channel state                                         */
/* ------------------------------------------------------------------ */

typedef enum { EG_ATTACK, EG_DECAY, EG_SUSTAIN, EG_RELEASE } eg_phase_t;

typedef struct {
    uint32_t phase;
    uint32_t freq_inc;

    uint8_t  multiple;
    uint8_t  detune;
    uint8_t  total_level;
    uint8_t  key_scale;
    uint8_t  attack_rate;
    uint8_t  decay_rate;
    uint8_t  sustain_rate;
    uint8_t  release_rate;
    uint8_t  sustain_level;
    uint8_t  am_enable;

    eg_phase_t eg_phase;
    uint16_t   eg_level;
    uint32_t   eg_counter;
} ym_op_t;

typedef struct {
    ym_op_t op[YM_OPERATORS];
    uint16_t fnum;
    uint8_t  block;
    uint8_t  keycode;       /* 5-bit: (block << 2) | (fnum >> 9) */
    uint8_t  algorithm;
    uint8_t  feedback;
    uint8_t  pan_left;
    uint8_t  pan_right;
    uint8_t  key_on;
    int32_t  fb_out[2];
} ym_ch_t;

static struct {
    ym_ch_t ch[YM_CHANNELS];
    uint8_t regs[2][0x100];
    uint8_t addr_latch[2];
    uint8_t dac_data;
    uint8_t dac_enable;
    uint8_t lfo_enable;
    uint8_t lfo_freq;
    uint8_t status;
    uint32_t phase_acc;
    uint32_t eg_timer;      /* global EG timer (counts at FM rate / 3) */
    uint8_t  eg_div3;       /* 0-2 divider for EG timer */
    int      tables_built;

    /* Timers */
    uint16_t timer_a;       /* Timer A 10-bit reload value (regs $24/$25) */
    uint8_t  timer_b;       /* Timer B 8-bit reload value (reg $26) */
    int32_t  timer_a_cnt;   /* Timer A down-counter (period = 1024 - timer_a) */
    int32_t  timer_b_cnt;   /* Timer B down-counter (period = (256 - timer_b) << 4) */
    uint8_t  timer_a_enable;/* Timer A overflow sets status flag */
    uint8_t  timer_b_enable;/* Timer B overflow sets status flag */
    uint8_t  ch3_mode;      /* Channel 3 special frequency mode */

    /* debug counters */
    int      dbg_dac_writes;
    int      dbg_ym_writes;   /* total data register writes per frame */
    int      dbg_frame;
} ym;

/* ------------------------------------------------------------------ */
/*  Init / Reset                                                       */
/* ------------------------------------------------------------------ */

void ym2612_init(void)
{
    memset(&ym, 0, sizeof(ym));
    for (int c = 0; c < YM_CHANNELS; c++) {
        ym.ch[c].pan_left = 1;
        ym.ch[c].pan_right = 1;
        for (int o = 0; o < YM_OPERATORS; o++) {
            ym.ch[c].op[o].eg_level = ENV_QUIET;
            ym.ch[c].op[o].eg_phase = EG_RELEASE;
        }
    }
    if (!ym.tables_built) {
        build_tables();
        ym.tables_built = 1;
    }
}

void ym2612_reset(void)
{
    int saved = ym.tables_built;
    memset(&ym, 0, sizeof(ym));
    ym.tables_built = saved;
    for (int c = 0; c < YM_CHANNELS; c++) {
        ym.ch[c].pan_left = 1;
        ym.ch[c].pan_right = 1;
        for (int o = 0; o < YM_OPERATORS; o++) {
            ym.ch[c].op[o].eg_level = ENV_QUIET;
            ym.ch[c].op[o].eg_phase = EG_RELEASE;
        }
    }
}

/* ------------------------------------------------------------------ */
/*  Frequency calculation                                              */
/* ------------------------------------------------------------------ */

static void update_keycode(ym_ch_t *ch)
{
    ch->keycode = ((ch->block & 7) << 2) | ((ch->fnum >> 9) & 3);
}

static void update_freq(ym_ch_t *ch, int op_idx)
{
    ym_op_t *op = &ch->op[op_idx];
    uint32_t fnum = ch->fnum;
    uint32_t block = ch->block;

    uint32_t base = (fnum << block) >> 1;
    if (op->multiple == 0)
        op->freq_inc = base >> 1;
    else
        op->freq_inc = base * op->multiple;

    /* Apply proper detune from hardware table */
    int dt_idx = op->detune & 3;
    int kc = ch->keycode & 31;
    int32_t dt_val = dt_tab[dt_idx][kc];
    if (op->detune & 4) dt_val = -dt_val;
    op->freq_inc = (uint32_t)((int32_t)op->freq_inc + dt_val);
}

/* ------------------------------------------------------------------ */
/*  Key on/off                                                         */
/* ------------------------------------------------------------------ */

static void key_on(ym_ch_t *ch, int op_mask)
{
    for (int i = 0; i < YM_OPERATORS; i++) {
        if (op_mask & (1 << i)) {
            ym_op_t *op = &ch->op[i];
            if (!(ch->key_on & (1 << i))) {
                op->phase = 0;
                op->eg_phase = EG_ATTACK;
                op->eg_level = ENV_QUIET;
                op->eg_counter = 0;
            }
        }
    }
    ch->key_on |= op_mask;
}

static void key_off(ym_ch_t *ch, int op_mask)
{
    for (int i = 0; i < YM_OPERATORS; i++) {
        if (op_mask & (1 << i)) {
            if (ch->key_on & (1 << i))
                ch->op[i].eg_phase = EG_RELEASE;
        }
    }
    ch->key_on &= ~op_mask;
}

/* ------------------------------------------------------------------ */
/*  Register write                                                     */
/* ------------------------------------------------------------------ */

static const int op_order[4] = { 0, 2, 1, 3 };

void ym2612_write(uint8_t port, uint8_t val)
{
    int part = (port >> 1) & 1;

    if (!(port & 1)) {
        ym.addr_latch[part] = val;
        return;
    }

    uint8_t reg = ym.addr_latch[part];
    ym.regs[part][reg] = val;
    ym.dbg_ym_writes++;

    /* Global registers (part 0 only, $20-$2F) */
    if (part == 0 && reg >= 0x20 && reg < 0x30) {
        switch (reg) {
        case 0x22:
            ym.lfo_enable = (val >> 3) & 1;
            ym.lfo_freq = val & 7;
            break;
        case 0x24: /* Timer A MSB (bits 9-2) */
            ym.timer_a = (ym.timer_a & 0x003) | ((uint16_t)val << 2);
            break;
        case 0x25: /* Timer A LSB (bits 1-0) */
            ym.timer_a = (ym.timer_a & 0x3FC) | (val & 3);
            break;
        case 0x26: /* Timer B */
            ym.timer_b = val;
            break;
        case 0x27: { /* Timer control / Channel 3 mode */
            ym.ch3_mode = (val >> 6) & 3;
            /* Reset overflow flags */
            if (val & 0x10) ym.status &= ~0x01;
            if (val & 0x20) ym.status &= ~0x02;
            /* Timer enable (allow overflow to set status flag) */
            ym.timer_a_enable = (val >> 2) & 1;
            ym.timer_b_enable = (val >> 3) & 1;
            /* Load timers (start counting if not already running) */
            if (val & 0x01) {
                if (ym.timer_a_cnt <= 0)
                    ym.timer_a_cnt = 1024 - ym.timer_a;
            }
            if (val & 0x02) {
                if (ym.timer_b_cnt <= 0)
                    ym.timer_b_cnt = (256 - ym.timer_b) << 4;
            }
            break;
        }
        case 0x28: {
            int ch_idx = val & 3;
            if (ch_idx == 3) break;
            if (val & 4) ch_idx += 3;
            ym_ch_t *ch = &ym.ch[ch_idx];
            int on_mask = 0, off_mask = 0;
            for (int i = 0; i < 4; i++) {
                if (val & (0x10 << i))
                    on_mask |= (1 << i);
                else
                    off_mask |= (1 << i);
            }
            if (off_mask) key_off(ch, off_mask);
            if (on_mask) key_on(ch, on_mask);
            break;
        }
        case 0x2A:
            ym.dac_data = val;
            ym.dbg_dac_writes++;
            break;
        case 0x2B:
            ym.dac_enable = (val >> 7) & 1;
            break;
        }
        return;
    }

    if (reg < 0x30) return;

    int ch_base = reg & 3;
    if (ch_base == 3) return;
    int ch_idx = ch_base + part * 3;
    if (ch_idx >= YM_CHANNELS) return;
    ym_ch_t *ch = &ym.ch[ch_idx];

    if (reg >= 0x30 && reg < 0xA0) {
        int op_raw = ((reg - 0x30) >> 2) & 3;
        int op_idx = op_order[op_raw];
        if (op_idx >= YM_OPERATORS) return;
        ym_op_t *op = &ch->op[op_idx];

        int reg_group = (reg - 0x30) & 0xF0;
        switch (reg_group) {
        case 0x00:
            op->detune = (val >> 4) & 7;
            op->multiple = val & 0x0F;
            update_freq(ch, op_idx);
            break;
        case 0x10:
            op->total_level = val & 0x7F;
            break;
        case 0x20:
            op->key_scale = (val >> 6) & 3;
            op->attack_rate = val & 0x1F;
            break;
        case 0x30:
            op->am_enable = (val >> 7) & 1;
            op->decay_rate = val & 0x1F;
            break;
        case 0x40:
            op->sustain_rate = val & 0x1F;
            break;
        case 0x50:
            op->sustain_level = (val >> 4) & 0x0F;
            op->release_rate = val & 0x0F;
            break;
        case 0x60:
            break;
        }
    } else if (reg >= 0xA0 && reg <= 0xB6) {
        int reg_off = reg - 0xA0;
        if (reg_off < 4) {
            ch->fnum = (ch->fnum & 0x700) | val;
            update_keycode(ch);
            for (int o = 0; o < YM_OPERATORS; o++)
                update_freq(ch, o);
        } else if (reg_off < 8) {
            ch->fnum = (ch->fnum & 0xFF) | ((val & 7) << 8);
            ch->block = (val >> 3) & 7;
            update_keycode(ch);
        } else if (reg_off >= 0x10 && reg_off < 0x14) {
            ch->algorithm = val & 7;
            ch->feedback = (val >> 3) & 7;
        } else if (reg_off >= 0x14 && reg_off < 0x18) {
            ch->pan_left = (val >> 7) & 1;
            ch->pan_right = (val >> 6) & 1;
        }
    }
}

uint8_t ym2612_read(void)
{
    return ym.status;
}

/* ------------------------------------------------------------------ */
/*  Envelope generator                                                 */
/* ------------------------------------------------------------------ */

static void eg_step(ym_op_t *op, int keycode)
{
    int rate;
    switch (op->eg_phase) {
    case EG_ATTACK:  rate = op->attack_rate ? op->attack_rate * 2 + 1 : 0; break;
    case EG_DECAY:   rate = op->decay_rate * 2; break;
    case EG_SUSTAIN: rate = op->sustain_rate * 2; break;
    case EG_RELEASE: rate = (op->release_rate * 2) + 1; break;
    default:         rate = 0; break;
    }

    /* Apply key scale rate: higher notes get faster envelopes */
    if (rate > 0 && op->key_scale < 3) {
        int ks_shift = 3 - op->key_scale;
        rate += (keycode >> ks_shift);
    } else if (rate > 0 && op->key_scale == 3) {
        rate += keycode;  /* no shift = maximum effect */
    }

    if (rate <= 0) return;
    if (rate > 63) rate = 63;

    /* Shift determines how often the EG updates for this rate.
     * Lower shift = faster update. */
    int shift;
    if (rate < 48)
        shift = 12 - (rate >> 2);
    else
        shift = 0;

    /* Check if this tick should produce an increment */
    int cycle_pos = (ym.eg_timer >> shift) & 7;
    int inc_sel = rate & 3;
    if (!eg_inc[inc_sel][cycle_pos])
        return;

    /* For high rates (48+), use larger steps */
    int step_size = 1;
    if (rate >= 48)
        step_size = 1 << ((rate - 48) >> 2);

    switch (op->eg_phase) {
    case EG_ATTACK:
        if (rate >= 62) {
            op->eg_level = 0;
            op->eg_phase = EG_DECAY;
        } else {
            op->eg_level -= (op->eg_level >> 4) + 1;
            if ((int16_t)op->eg_level <= 0) {
                op->eg_level = 0;
                op->eg_phase = EG_DECAY;
            }
        }
        break;
    case EG_DECAY: {
        op->eg_level += step_size;
        int sl = op->sustain_level == 0x0F ? ENV_QUIET
               : (op->sustain_level << 5);
        if (op->eg_level >= (uint16_t)sl) {
            op->eg_level = sl;
            op->eg_phase = EG_SUSTAIN;
        }
        break;
    }
    case EG_SUSTAIN:
        op->eg_level += step_size;
        if (op->eg_level >= ENV_QUIET)
            op->eg_level = ENV_QUIET;
        break;
    case EG_RELEASE:
        op->eg_level += step_size;
        if (op->eg_level >= ENV_QUIET)
            op->eg_level = ENV_QUIET;
        break;
    }
}

/* ------------------------------------------------------------------ */
/*  Operator output                                                    */
/* ------------------------------------------------------------------ */

static int32_t op_output(ym_op_t *op, int32_t phase_mod)
{
    uint32_t phase = (op->phase + (uint32_t)phase_mod) >> 10;
    phase &= (SINE_TABLE_SIZE - 1);

    uint16_t sin_val = sin_table[phase & (SINE_TABLE_SIZE / 2 - 1)];
    uint32_t atten = sin_val + (op->eg_level << 2) + (op->total_level << 3);
    if (atten >= 4096) return 0;

    int32_t out = pow_table[atten & 0xFF] >> (atten >> 8);
    if (phase >= SINE_TABLE_SIZE / 2)
        out = -out;

    return out;
}

/* ------------------------------------------------------------------ */
/*  Channel synthesis: 8 algorithms                                    */
/* ------------------------------------------------------------------ */

static int32_t synth_channel(ym_ch_t *ch)
{
    ym_op_t *op = ch->op;

    for (int i = 0; i < YM_OPERATORS; i++)
        op[i].phase += op[i].freq_inc;

    /* EG only updates when the /3 divider ticks (eg_div3 wraps to 0) */
    if (ym.eg_div3 == 0) {
        for (int i = 0; i < YM_OPERATORS; i++)
            eg_step(&op[i], ch->keycode);
    }

    int32_t fb;
    if (ch->feedback)
        fb = (ch->fb_out[0] + ch->fb_out[1]) >> (10 - ch->feedback);
    else
        fb = 0;

    int32_t out1 = op_output(&op[0], fb);
    ch->fb_out[1] = ch->fb_out[0];
    ch->fb_out[0] = out1;

    int32_t out2, out3, out4, result;
    switch (ch->algorithm) {
    case 0:
        out2 = op_output(&op[1], out1 << 10);
        out3 = op_output(&op[2], out2 << 10);
        out4 = op_output(&op[3], out3 << 10);
        result = out4;
        break;
    case 1:
        out2 = op_output(&op[1], 0);
        out3 = op_output(&op[2], (out1 + out2) << 9);
        out4 = op_output(&op[3], out3 << 10);
        result = out4;
        break;
    case 2:
        out2 = op_output(&op[1], 0);
        out3 = op_output(&op[2], out2 << 10);
        out4 = op_output(&op[3], (out1 + out3) << 9);
        result = out4;
        break;
    case 3:
        out2 = op_output(&op[1], out1 << 10);
        out3 = op_output(&op[2], 0);
        out4 = op_output(&op[3], (out2 + out3) << 9);
        result = out4;
        break;
    case 4:
        out2 = op_output(&op[1], out1 << 10);
        out3 = op_output(&op[2], 0);
        out4 = op_output(&op[3], out3 << 10);
        result = out2 + out4;
        break;
    case 5:
        out2 = op_output(&op[1], out1 << 10);
        out3 = op_output(&op[2], out1 << 10);
        out4 = op_output(&op[3], out1 << 10);
        result = out2 + out3 + out4;
        break;
    case 6:
        out2 = op_output(&op[1], out1 << 10);
        out3 = op_output(&op[2], 0);
        out4 = op_output(&op[3], 0);
        result = out2 + out3 + out4;
        break;
    case 7:
    default:
        out2 = op_output(&op[1], 0);
        out3 = op_output(&op[2], 0);
        out4 = op_output(&op[3], 0);
        result = out1 + out2 + out3 + out4;
        break;
    }

    return result;
}

/* ------------------------------------------------------------------ */
/*  Sample generation                                                  */
/* ------------------------------------------------------------------ */

static void ym_tick(int32_t *left, int32_t *right)
{
    /* Advance global EG timer (runs at FM rate / 3) */
    ym.eg_div3++;
    if (ym.eg_div3 >= 3) {
        ym.eg_div3 = 0;
        ym.eg_timer++;
    }

    /* Timer A: counts down once per FM sample */
    if (ym.timer_a_cnt > 0) {
        ym.timer_a_cnt--;
        if (ym.timer_a_cnt <= 0) {
            if (ym.timer_a_enable)
                ym.status |= 0x01;
            ym.timer_a_cnt = 1024 - ym.timer_a;
        }
    }

    /* Timer B: period pre-scaled by 16, also counts once per FM sample */
    if (ym.timer_b_cnt > 0) {
        ym.timer_b_cnt--;
        if (ym.timer_b_cnt <= 0) {
            if (ym.timer_b_enable)
                ym.status |= 0x02;
            ym.timer_b_cnt = (256 - ym.timer_b) << 4;
        }
    }

    int32_t l = 0, r = 0;

    for (int c = 0; c < YM_CHANNELS; c++) {
        int32_t out;
        if (c == 5 && ym.dac_enable) {
            out = ((int32_t)ym.dac_data - 128) << 6;
        } else {
            out = synth_channel(&ym.ch[c]);
        }

        if (ym.ch[c].pan_left)  l += out;
        if (ym.ch[c].pan_right) r += out;
    }

    *left = l;
    *right = r;
}

void ym2612_debug_frame(void)
{
    ym.dbg_frame++;
    (void)z80_debug_steps();
    ym.dbg_dac_writes = 0;
    ym.dbg_ym_writes = 0;
}

void ym2612_run_samples(int32_t *buf, int count, int sample_rate)
{
    uint32_t step = (uint32_t)(((uint64_t)YM_NATIVE_RATE << 16) / sample_rate);

    for (int i = 0; i < count; i++) {
        int32_t l_acc = 0, r_acc = 0;
        int ticks = 0;

        ym.phase_acc += step;
        int whole = ym.phase_acc >> 16;
        ym.phase_acc &= 0xFFFF;

        for (int t = 0; t < whole; t++) {
            int32_t l, r;
            ym_tick(&l, &r);
            l_acc += l;
            r_acc += r;
            ticks++;
        }
        if (!ticks) {
            int32_t l, r;
            ym_tick(&l, &r);
            l_acc = l;
            r_acc = r;
            ticks = 1;
        }

        buf[i * 2]     += l_acc / ticks / 2;
        buf[i * 2 + 1] += r_acc / ticks / 2;
    }
}

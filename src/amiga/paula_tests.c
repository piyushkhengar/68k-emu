/*
 * Unit tests for the Amiga 500 Paula chip (src/amiga/paula.c).
 *
 * Tests exercise the register model and direct-mode audio tick.
 * DMA fetch from chip RAM is out of scope for Phase 2 — all tick
 * tests use AUDnDAT writes to inject samples without DMA.
 *
 * Phase 2 register coverage:
 *   - DMACON / DMACONR   (SET/CLR mechanism)
 *   - INTENA / INTENAR   (SET/CLR mechanism)
 *   - INTREQ / INTREQR   (SET/CLR mechanism)
 *   - ADKCON / ADKCONR   (SET/CLR mechanism)
 *   - AUDnLCH/LCL        (24-bit DMA pointer assembly)
 *   - AUDnLEN            (word count)
 *   - AUDnPER            (period)
 *   - AUDnVOL            (volume with clamp at 64)
 *   - AUDnDAT            (direct sample + period-counter load)
 *
 * Tick coverage:
 *   - Silence when per = 0
 *   - Silence when vol = 0
 *   - Direct-mode output: sample * vol / 64
 *   - Volume scaling
 *   - Period counter delays first output
 *   - Period counter re-fires and holds output
 *   - Stereo routing: ch0+ch3 → left, ch1+ch2 → right
 *   - Multi-channel mix
 *   - Negative (signed) samples
 */

#include "paula.h"
#include "paula_tests.h"
#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/*  Test framework (same style as bus_tests.c)                          */
/* ------------------------------------------------------------------ */

static int failures;
static int total;

#define BASSERT(expr, fmt, ...) do {                                    \
    total++;                                                            \
    if (!(expr)) {                                                      \
        failures++;                                                     \
        printf("    FAIL [%s:%d]: " fmt "\n", __func__, __LINE__,      \
               ##__VA_ARGS__);                                          \
    }                                                                   \
} while (0)

/* ------------------------------------------------------------------ */
/*  Register offset constants (relative to 0xDFF000)                    */
/* ------------------------------------------------------------------ */

#define DMACONR  0x002u
#define ADKCONR  0x010u
#define INTENAR  0x01Cu
#define INTREQR  0x01Eu
#define DMACON   0x096u
#define INTENA   0x09Au
#define INTREQ   0x09Cu
#define ADKCON   0x09Eu

/* Channel bases: 0x0A0, 0x0B0, 0x0C0, 0x0D0 */
#define AUD_BASE(n)   (0x0A0u + (unsigned)(n) * 0x10u)
#define AUDnLCH(n)    (AUD_BASE(n) + 0x0u)
#define AUDnLCL(n)    (AUD_BASE(n) + 0x2u)
#define AUDnLEN(n)    (AUD_BASE(n) + 0x4u)
#define AUDnPER(n)    (AUD_BASE(n) + 0x6u)
#define AUDnVOL(n)    (AUD_BASE(n) + 0x8u)
#define AUDnDAT(n)    (AUD_BASE(n) + 0xAu)

/* ------------------------------------------------------------------ */
/*  Helpers                                                             */
/* ------------------------------------------------------------------ */

static paula_t p;

static void reset_paula(void)
{
    paula_init(&p);
}

/*
 * Set up a channel for direct-mode tick tests:
 *   per must be written before dat so per_cnt is loaded correctly.
 */
static void setup_channel(int ch, uint8_t vol, uint16_t per, int8_t sample)
{
    paula_write_reg(&p, AUDnVOL(ch), vol);
    paula_write_reg(&p, AUDnPER(ch), per);
    /* Pack sample into high byte of AUDnDAT; low byte = 0. */
    paula_write_reg(&p, AUDnDAT(ch), (uint16_t)((uint8_t)sample) << 8);
}

/* ------------------------------------------------------------------ */
/*  Group 1: Lifecycle                                                  */
/* ------------------------------------------------------------------ */

static void test_init_zeroes_channels(void)
{
    reset_paula();
    for (int i = 0; i < 4; i++) {
        BASSERT(p.ch[i].lc         == 0,   "ch[%d].lc != 0 after init",        i);
        BASSERT(p.ch[i].len        == 0,   "ch[%d].len != 0 after init",       i);
        BASSERT(p.ch[i].per        == 0,   "ch[%d].per != 0 after init",       i);
        BASSERT(p.ch[i].vol        == 0,   "ch[%d].vol != 0 after init",       i);
        BASSERT(p.ch[i].sample     == 0,   "ch[%d].sample != 0 after init",    i);
        BASSERT(p.ch[i].per_cnt    == 0,   "ch[%d].per_cnt != 0 after init",   i);
        BASSERT(p.ch[i].out_sample == 0,   "ch[%d].out_sample != 0 after init",i);
    }
}

static void test_init_zeroes_control_regs(void)
{
    reset_paula();
    BASSERT(p.dmacon == 0, "dmacon != 0 after init (got 0x%04X)", p.dmacon);
    BASSERT(p.adkcon == 0, "adkcon != 0 after init (got 0x%04X)", p.adkcon);
    BASSERT(p.intena == 0, "intena != 0 after init (got 0x%04X)", p.intena);
    BASSERT(p.intreq == 0, "intreq != 0 after init (got 0x%04X)", p.intreq);
}

static void test_reset_clears_state(void)
{
    reset_paula();
    /* Dirty some state. */
    paula_write_reg(&p, DMACON,    0x8209);
    paula_write_reg(&p, AUDnVOL(0), 42);
    paula_write_reg(&p, AUDnPER(1), 128);
    /* Reset must zero everything. */
    paula_reset(&p);
    BASSERT(p.dmacon        == 0, "dmacon != 0 after reset");
    BASSERT(p.ch[0].vol     == 0, "ch[0].vol != 0 after reset");
    BASSERT(p.ch[1].per     == 0, "ch[1].per != 0 after reset");
    BASSERT(p.ch[0].per_cnt == 0, "ch[0].per_cnt != 0 after reset");
}

/* ------------------------------------------------------------------ */
/*  Group 2: SET/CLR register mechanism — DMACON                       */
/* ------------------------------------------------------------------ */

static void test_dmacon_set_bits(void)
{
    reset_paula();
    /* Bit 15 = 1 → SET mode: set bits 9, 3, 0 */
    paula_write_reg(&p, DMACON, 0x8000 | (1u << 9) | (1u << 3) | (1u << 0));
    BASSERT((p.dmacon >> 9) & 1, "dmacon bit 9 not set");
    BASSERT((p.dmacon >> 3) & 1, "dmacon bit 3 not set");
    BASSERT((p.dmacon >> 0) & 1, "dmacon bit 0 not set");
    /* Bits we did not set must remain 0. */
    BASSERT(!(p.dmacon & 0x0002), "dmacon bit 1 spuriously set (0x%04X)", p.dmacon);
}

static void test_dmacon_clear_bits(void)
{
    reset_paula();
    /* Set bits 9, 3, 0. */
    paula_write_reg(&p, DMACON, 0x8000 | (1u << 9) | (1u << 3) | (1u << 0));
    /* Bit 15 = 0 → CLR mode: clear bits 9 and 0, leave bit 3. */
    paula_write_reg(&p, DMACON, 0x0000 | (1u << 9) | (1u << 0));
    BASSERT(!((p.dmacon >> 9) & 1), "dmacon bit 9 should be cleared");
    BASSERT(!((p.dmacon >> 0) & 1), "dmacon bit 0 should be cleared");
    BASSERT(  (p.dmacon >> 3) & 1,  "dmacon bit 3 should still be set");
}

static void test_dmaconr_reflects_dmacon(void)
{
    reset_paula();
    paula_write_reg(&p, DMACON, 0x8000 | 0x020F); /* set bits 9, 3:0 */
    uint16_t r = paula_read_reg(&p, DMACONR);
    BASSERT(r == (p.dmacon),
            "DMACONR (0x%04X) does not match dmacon (0x%04X)", r, p.dmacon);
}

/* ------------------------------------------------------------------ */
/*  Group 3: SET/CLR — INTENA / INTENAR                                */
/* ------------------------------------------------------------------ */

static void test_intena_set_clear(void)
{
    reset_paula();
    /* Set bit 7 (audio channel 0 interrupt enable). */
    paula_write_reg(&p, INTENA, 0x8080);
    BASSERT((p.intena >> 7) & 1, "intena bit 7 not set after 0x8080 write");
    /* Clear bit 7. */
    paula_write_reg(&p, INTENA, 0x0080);
    BASSERT(!((p.intena >> 7) & 1), "intena bit 7 not cleared after 0x0080 write");
}

static void test_intenar_reflects_intena(void)
{
    reset_paula();
    paula_write_reg(&p, INTENA, 0x8780); /* set bits 10:7 */
    uint16_t r = paula_read_reg(&p, INTENAR);
    BASSERT(r == p.intena,
            "INTENAR (0x%04X) does not match intena (0x%04X)", r, p.intena);
}

/* ------------------------------------------------------------------ */
/*  Group 4: SET/CLR — INTREQ / INTREQR                                */
/* ------------------------------------------------------------------ */

static void test_intreq_set_clear(void)
{
    reset_paula();
    paula_write_reg(&p, INTREQ, 0x8080);
    BASSERT((p.intreq >> 7) & 1, "intreq bit 7 not set");
    paula_write_reg(&p, INTREQ, 0x0080);
    BASSERT(!((p.intreq >> 7) & 1), "intreq bit 7 not cleared");
}

static void test_intreqr_reflects_intreq(void)
{
    reset_paula();
    paula_write_reg(&p, INTREQ, 0x8380); /* set bits 9:7 */
    uint16_t r = paula_read_reg(&p, INTREQR);
    BASSERT(r == p.intreq,
            "INTREQR (0x%04X) does not match intreq (0x%04X)", r, p.intreq);
}

/* ------------------------------------------------------------------ */
/*  Group 5: SET/CLR — ADKCON / ADKCONR                                */
/* ------------------------------------------------------------------ */

static void test_adkcon_set_clear(void)
{
    reset_paula();
    paula_write_reg(&p, ADKCON, 0x8100); /* set bit 8 (UARTBRK) */
    BASSERT((p.adkcon >> 8) & 1, "adkcon bit 8 not set");
    paula_write_reg(&p, ADKCON, 0x0100); /* clear bit 8 */
    BASSERT(!((p.adkcon >> 8) & 1), "adkcon bit 8 not cleared");
}

static void test_adkconr_reflects_adkcon(void)
{
    reset_paula();
    paula_write_reg(&p, ADKCON, 0x8FF0);
    uint16_t r = paula_read_reg(&p, ADKCONR);
    BASSERT(r == p.adkcon,
            "ADKCONR (0x%04X) does not match adkcon (0x%04X)", r, p.adkcon);
}

/* ------------------------------------------------------------------ */
/*  Group 6: Per-channel register writes                                */
/* ------------------------------------------------------------------ */

static void test_aud_len_stored(void)
{
    reset_paula();
    paula_write_reg(&p, AUDnLEN(0), 0x1234);
    BASSERT(p.ch[0].len == 0x1234,
            "ch[0].len: expected 0x1234, got 0x%04X", p.ch[0].len);
}

static void test_aud_per_stored(void)
{
    reset_paula();
    paula_write_reg(&p, AUDnPER(2), 160);
    BASSERT(p.ch[2].per == 160,
            "ch[2].per: expected 160, got %u", p.ch[2].per);
}

static void test_aud_vol_stored(void)
{
    reset_paula();
    paula_write_reg(&p, AUDnVOL(1), 32);
    BASSERT(p.ch[1].vol == 32,
            "ch[1].vol: expected 32, got %u", p.ch[1].vol);
}

static void test_aud_vol_clamped_at_64(void)
{
    reset_paula();
    paula_write_reg(&p, AUDnVOL(0), 128); /* 128 > 64: must clamp */
    BASSERT(p.ch[0].vol == 64,
            "ch[0].vol: expected 64 (clamped from 128), got %u", p.ch[0].vol);
    paula_write_reg(&p, AUDnVOL(0), 255); /* max uint8, still clamped */
    BASSERT(p.ch[0].vol == 64,
            "ch[0].vol: expected 64 (clamped from 255), got %u", p.ch[0].vol);
}

static void test_aud_vol_max_exact(void)
{
    reset_paula();
    paula_write_reg(&p, AUDnVOL(3), 64); /* 64 is valid, must not be clamped */
    BASSERT(p.ch[3].vol == 64,
            "ch[3].vol: expected 64, got %u", p.ch[3].vol);
}

static void test_aud_lc_combined(void)
{
    reset_paula();
    /* High 3 bits of the 24-bit pointer via LCH; full low 16 via LCL. */
    paula_write_reg(&p, AUDnLCH(0), 0x0003); /* bits [23:16] = 0x03 */
    paula_write_reg(&p, AUDnLCL(0), 0x1234); /* bits [15:0]  = 0x1234 */
    BASSERT(p.ch[0].lc == 0x00031234u,
            "ch[0].lc: expected 0x00031234, got 0x%08X", p.ch[0].lc);
}

static void test_aud_dat_sets_sample(void)
{
    reset_paula();
    /* High byte of AUDnDAT = 0x7F → sample should be 0x7F (127). */
    paula_write_reg(&p, AUDnDAT(0), 0x7F00);
    BASSERT(p.ch[0].sample == 0x7F,
            "ch[0].sample: expected 0x7F, got 0x%02X", (uint8_t)p.ch[0].sample);
}

static void test_aud_dat_loads_per_cnt(void)
{
    reset_paula();
    paula_write_reg(&p, AUDnPER(0), 42);
    paula_write_reg(&p, AUDnDAT(0), 0x1000);
    BASSERT(p.ch[0].per_cnt == 42,
            "ch[0].per_cnt: expected 42 (loaded from per), got %u",
            p.ch[0].per_cnt);
}

/* ------------------------------------------------------------------ */
/*  Group 7: Channel independence                                       */
/* ------------------------------------------------------------------ */

static void test_all_channels_independent(void)
{
    reset_paula();
    paula_write_reg(&p, AUDnPER(0), 100);
    paula_write_reg(&p, AUDnPER(1), 200);
    paula_write_reg(&p, AUDnPER(2), 300);
    paula_write_reg(&p, AUDnPER(3), 400);
    BASSERT(p.ch[0].per == 100, "ch[0].per: expected 100, got %u", p.ch[0].per);
    BASSERT(p.ch[1].per == 200, "ch[1].per: expected 200, got %u", p.ch[1].per);
    BASSERT(p.ch[2].per == 300, "ch[2].per: expected 300, got %u", p.ch[2].per);
    BASSERT(p.ch[3].per == 400, "ch[3].per: expected 400, got %u", p.ch[3].per);
}

/* ------------------------------------------------------------------ */
/*  Group 8: Tick — direct mode                                         */
/* ------------------------------------------------------------------ */

static void test_tick_silence_per_zero(void)
{
    reset_paula();
    /* per = 0: channel must never fire regardless of vol/sample. */
    paula_write_reg(&p, AUDnVOL(0), 64);
    paula_write_reg(&p, AUDnPER(0), 0);
    paula_write_reg(&p, AUDnDAT(0), 0x7F00);
    int16_t left = 99, right = 99;
    paula_tick(&p, 10, &left, &right);
    BASSERT(left  == 0, "tick with per=0: left expected 0, got %d",  (int)left);
    BASSERT(right == 0, "tick with per=0: right expected 0, got %d", (int)right);
}

static void test_tick_silence_vol_zero(void)
{
    reset_paula();
    /* vol = 0: output must be 0 regardless of sample. */
    setup_channel(0, 0, 1, 127);
    int16_t left = 99, right = 99;
    paula_tick(&p, 1, &left, &right);
    BASSERT(left  == 0, "tick vol=0: left expected 0, got %d",  (int)left);
    BASSERT(right == 0, "tick vol=0: right expected 0, got %d", (int)right);
}

static void test_tick_direct_output(void)
{
    reset_paula();
    /* ch0: vol=64, per=1, sample=100; after 1 clock the period fires → left=100. */
    setup_channel(0, 64, 1, 100);
    int16_t left = 0, right = 0;
    paula_tick(&p, 1, &left, &right);
    BASSERT(left == 100,
            "tick direct: left expected 100, got %d", (int)left);
    BASSERT(right == 0,
            "tick direct: right expected 0, got %d", (int)right);
}

static void test_tick_volume_scaling(void)
{
    reset_paula();
    /* vol=32, sample=64: output = 64 * 32 / 64 = 32. */
    setup_channel(0, 32, 1, 64);
    int16_t left = 0, right = 0;
    paula_tick(&p, 1, &left, &right);
    BASSERT(left == 32,
            "tick vol-scale: left expected 32, got %d", (int)left);
}

static void test_tick_period_delays_first_output(void)
{
    reset_paula();
    /* per=2: first fire happens after 2 clocks. */
    setup_channel(0, 64, 2, 50);
    int16_t left = 0, right = 0;

    /* Tick 1: per_cnt 2→1, no fire yet. */
    paula_tick(&p, 1, &left, &right);
    BASSERT(left == 0,
            "period delay tick 1: left expected 0, got %d", (int)left);

    /* Tick 2: per_cnt 1→0, fires — out_sample = 50. */
    paula_tick(&p, 1, &left, &right);
    BASSERT(left == 50,
            "period delay tick 2: left expected 50, got %d", (int)left);
}

static void test_tick_output_held_between_fires(void)
{
    reset_paula();
    /* per=3, sample=80: after first fire (3 clocks) out_sample=80 and holds. */
    setup_channel(0, 64, 3, 80);
    int16_t left = 0, right = 0;

    paula_tick(&p, 3, &left, &right); /* first fire */
    BASSERT(left == 80, "tick held: first fire expected 80, got %d", (int)left);

    /* In the 4th clock (per_cnt = 3→2) the sample should still be 80. */
    paula_tick(&p, 1, &left, &right);
    BASSERT(left == 80, "tick held: output should still be 80, got %d", (int)left);
}

static void test_tick_refires_after_period(void)
{
    reset_paula();
    /* per=1: fires every clock; after 5 clocks out_sample is still 70. */
    setup_channel(0, 64, 1, 70);
    int16_t left = 0, right = 0;
    paula_tick(&p, 5, &left, &right);
    BASSERT(left == 70,
            "tick refire: left expected 70, got %d", (int)left);
}

static void test_tick_stereo_routing(void)
{
    reset_paula();
    /*
     * Hardware routing: left = ch0 + ch3, right = ch1 + ch2.
     * Use vol=64, per=1 so each channel fires on the first clock.
     */
    setup_channel(0, 64, 1, 10); /* → left  */
    setup_channel(1, 64, 1, 20); /* → right */
    setup_channel(2, 64, 1, 30); /* → right */
    setup_channel(3, 64, 1, 40); /* → left  */
    int16_t left = 0, right = 0;
    paula_tick(&p, 1, &left, &right);
    BASSERT(left  == 50, "stereo routing: left  expected 50 (10+40), got %d", (int)left);
    BASSERT(right == 50, "stereo routing: right expected 50 (20+30), got %d", (int)right);
}

static void test_tick_multi_channel_mix(void)
{
    reset_paula();
    /* Only ch0 (left) and ch1 (right) active. */
    setup_channel(0, 64, 1, 30);
    setup_channel(1, 64, 1, 10);
    int16_t left = 0, right = 0;
    paula_tick(&p, 1, &left, &right);
    BASSERT(left  == 30, "mix: left  expected 30, got %d", (int)left);
    BASSERT(right == 10, "mix: right expected 10, got %d", (int)right);
}

static void test_tick_negative_sample(void)
{
    reset_paula();
    /*
     * 0x80 interpreted as int8_t = -128.
     * output = -128 * 64 / 64 = -128.
     */
    paula_write_reg(&p, AUDnVOL(0), 64);
    paula_write_reg(&p, AUDnPER(0), 1);
    paula_write_reg(&p, AUDnDAT(0), 0x8000); /* high byte = 0x80 = -128 */
    int16_t left = 0, right = 0;
    paula_tick(&p, 1, &left, &right);
    BASSERT(left == -128,
            "negative sample: left expected -128, got %d", (int)left);
}

/* ------------------------------------------------------------------ */
/*  Entry point                                                         */
/* ------------------------------------------------------------------ */

int run_paula_tests(void)
{
    failures = 0;
    total    = 0;

    printf("Paula tests:\n");

    /* Lifecycle */
    test_init_zeroes_channels();
    test_init_zeroes_control_regs();
    test_reset_clears_state();

    /* DMACON SET/CLR */
    test_dmacon_set_bits();
    test_dmacon_clear_bits();
    test_dmaconr_reflects_dmacon();

    /* INTENA SET/CLR */
    test_intena_set_clear();
    test_intenar_reflects_intena();

    /* INTREQ SET/CLR */
    test_intreq_set_clear();
    test_intreqr_reflects_intreq();

    /* ADKCON SET/CLR */
    test_adkcon_set_clear();
    test_adkconr_reflects_adkcon();

    /* Per-channel registers */
    test_aud_len_stored();
    test_aud_per_stored();
    test_aud_vol_stored();
    test_aud_vol_clamped_at_64();
    test_aud_vol_max_exact();
    test_aud_lc_combined();
    test_aud_dat_sets_sample();
    test_aud_dat_loads_per_cnt();

    /* Channel independence */
    test_all_channels_independent();

    /* Tick — direct mode */
    test_tick_silence_per_zero();
    test_tick_silence_vol_zero();
    test_tick_direct_output();
    test_tick_volume_scaling();
    test_tick_period_delays_first_output();
    test_tick_output_held_between_fires();
    test_tick_refires_after_period();
    test_tick_stereo_routing();
    test_tick_multi_channel_mix();
    test_tick_negative_sample();

    if (failures == 0)
        printf("  Paula: all %d assertions passed.\n", total);
    else
        printf("  Paula: %d/%d assertions FAILED.\n", failures, total);

    return failures;
}

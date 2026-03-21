/*
 * SN76489 PSG emulator for the Genesis.
 *
 * 3 tone channels produce square waves at programmable frequencies.
 * 1 noise channel produces white or periodic noise.  Each channel has
 * a 4-bit volume attenuation (0 = loudest, 15 = off).
 *
 * The chip uses a latch/data register write protocol:
 *   Bit 7 = 1: latch byte -- selects channel (bits 6-5) and register
 *              type (bit 4: 0=tone/noise, 1=volume), plus low 4 data bits.
 *   Bit 7 = 0: data byte -- provides 6 additional high bits for the
 *              currently latched register.
 *
 * Internal clock: PSG_CLOCK / 16 ≈ 223,721 Hz output rate, downsampled
 * to the host audio rate via box-filter averaging plus a 1st-order IIR
 * low-pass filter to suppress aliasing artifacts.
 */

#include "psg.h"
#include <string.h>

#define PSG_CLOCK  3579545
#define PSG_DIVISOR 16

/* Volume table: 4-bit attenuation -> linear amplitude (0-8191).
 * Each step is -2 dB; entry 15 = silence. */
static const int16_t vol_table[16] = {
    8191, 6507, 5168, 4105, 3261, 2590, 2057, 1634,
    1298, 1031,  819,  650,  516,  410,  326,    0,
};

static struct {
    uint16_t tone_freq[3];
    int16_t  tone_counter[3];
    int8_t   tone_polarity[3];

    uint8_t  noise_ctrl;
    uint16_t noise_shift;
    int16_t  noise_counter;
    int8_t   noise_polarity;

    uint8_t  volume[4];

    uint8_t  latched_ch;
    uint8_t  latched_is_vol;

    uint32_t phase_acc;

    /* 1st-order IIR low-pass filter state (fixed-point 16.16) */
    int32_t lpf_state;
} psg;

void psg_init(void)
{
    memset(&psg, 0, sizeof(psg));
    for (int i = 0; i < 4; i++)
        psg.volume[i] = 0x0F;
    psg.tone_polarity[0] = 1;
    psg.tone_polarity[1] = 1;
    psg.tone_polarity[2] = 1;
    psg.noise_polarity = 1;
    psg.noise_shift = 0x8000;
}

void psg_reset(void)
{
    psg_init();
}

static int noise_freq_from_ctrl(void)
{
    switch (psg.noise_ctrl & 3) {
    case 0: return 0x10;
    case 1: return 0x20;
    case 2: return 0x40;
    default: return psg.tone_freq[2];
    }
}

void psg_write(uint8_t val)
{
    if (val & 0x80) {
        int ch = (val >> 5) & 3;
        int is_vol = (val >> 4) & 1;
        psg.latched_ch = ch;
        psg.latched_is_vol = is_vol;

        if (is_vol) {
            psg.volume[ch] = val & 0x0F;
        } else if (ch == 3) {
            psg.noise_ctrl = val & 0x07;
            psg.noise_shift = 0x8000;
        } else {
            psg.tone_freq[ch] = (psg.tone_freq[ch] & 0x3F0) | (val & 0x0F);
        }
    } else {
        int ch = psg.latched_ch;
        if (psg.latched_is_vol) {
            psg.volume[ch] = val & 0x0F;
        } else if (ch == 3) {
            psg.noise_ctrl = val & 0x07;
            psg.noise_shift = 0x8000;
        } else {
            psg.tone_freq[ch] = (psg.tone_freq[ch] & 0x0F) |
                                ((val & 0x3F) << 4);
        }
    }
}

static int32_t psg_tick(void)
{
    int32_t out = 0;

    for (int ch = 0; ch < 3; ch++) {
        psg.tone_counter[ch]--;
        if (psg.tone_counter[ch] <= 0) {
            psg.tone_counter[ch] = psg.tone_freq[ch] ? psg.tone_freq[ch] : 1;
            psg.tone_polarity[ch] = -psg.tone_polarity[ch];
        }
        out += vol_table[psg.volume[ch]] * psg.tone_polarity[ch];
    }

    psg.noise_counter--;
    if (psg.noise_counter <= 0) {
        psg.noise_counter = noise_freq_from_ctrl();
        if (!psg.noise_counter) psg.noise_counter = 1;
        psg.noise_polarity = -psg.noise_polarity;
        if (psg.noise_polarity > 0) {
            int tap;
            if (psg.noise_ctrl & 0x04)
                tap = (psg.noise_shift & 1) ^ ((psg.noise_shift >> 3) & 1);
            else
                tap = psg.noise_shift & 1;
            psg.noise_shift = (psg.noise_shift >> 1) | (tap << 15);
        }
    }
    out += vol_table[psg.volume[3]] * ((psg.noise_shift & 1) ? 1 : -1);

    return out >> 2;
}

void psg_run_samples(int32_t *buf, int count, int sample_rate)
{
    uint32_t psg_rate = PSG_CLOCK / PSG_DIVISOR;
    uint32_t step = (uint32_t)(((uint64_t)psg_rate << 16) / sample_rate);

    /*
     * IIR coefficient for ~12 kHz cutoff at the output sample rate.
     * alpha = fc / (fc + fs/(2*pi)), in 16.16 fixed point.
     * For fs=44100, fc=12000: alpha ~= 0.63 -> 41288 in 16.16
     */
    int32_t alpha;
    if (sample_rate > 0)
        alpha = (int32_t)(((int64_t)12000 * 65536) /
                          (12000 + sample_rate * 10000 / 62832));
    else
        alpha = 65536;

    for (int i = 0; i < count; i++) {
        int32_t acc = 0;
        int ticks = 0;
        psg.phase_acc += step;
        int whole = psg.phase_acc >> 16;
        psg.phase_acc &= 0xFFFF;
        for (int t = 0; t < whole; t++) {
            acc += psg_tick();
            ticks++;
        }
        if (!ticks) ticks = 1;
        int32_t raw = acc / ticks;

        /* 1st-order IIR low-pass: y += alpha * (x - y) */
        psg.lpf_state += (int32_t)(((int64_t)(raw - psg.lpf_state) * alpha) >> 16);
        int32_t sample = psg.lpf_state;

        buf[i * 2]     += sample;
        buf[i * 2 + 1] += sample;
    }
}

/*
 * Denise — Chapter 3 step 2: colour registers + bitplane pixel pipeline.
 *
 * Sprites, HAM/EHB, dual-playfield, and horizontal scroll are deferred.
 */

#include "denise.h"
#include <string.h>

/* ------------------------------------------------------------------ */
/*  Lifecycle                                                           */
/* ------------------------------------------------------------------ */

void denise_init(denise_t *d)
{
    memset(d, 0, sizeof(*d));
}

void denise_reset(denise_t *d)
{
    memset(d, 0, sizeof(*d));
}

/* ------------------------------------------------------------------ */
/*  Colour expansion                                                    */
/* ------------------------------------------------------------------ */

uint32_t denise_expand_color(uint16_t rgb12)
{
    /*
     * The Amiga palette register stores one colour as 0x0RGB:
     *   bits 11:8 = red   nibble
     *   bits  7:4 = green nibble
     *   bits  3:0 = blue  nibble
     *
     * The DAC replicates each nibble to fill a full byte:
     *   0xF → 0xFF   (1111 → 11111111)
     *   0x8 → 0x88   (1000 → 10001000)
     *   0x0 → 0x00   (0000 → 00000000)
     *
     * This is NOT the same as scaling 0–15 to 0–255 linearly
     * (which would give 0xF → 0xFF, 0x8 → 0x88 anyway, but 0x7 → 0x77
     * rather than the linear value 0x77 = 7*255/15 ≈ 119 = 0x77 — they
     * happen to match for 4-bit because 2^4-1 = 15 and 2^8-1 = 255 = 17*15).
     * Replicate is the correct model for the real hardware.
     */
    uint8_t r4 = (rgb12 >> 8) & 0xFu;
    uint8_t g4 = (rgb12 >> 4) & 0xFu;
    uint8_t b4 = (rgb12 >> 0) & 0xFu;

    uint8_t r8 = (uint8_t)((r4 << 4) | r4);
    uint8_t g8 = (uint8_t)((g4 << 4) | g4);
    uint8_t b8 = (uint8_t)((b4 << 4) | b4);

    return 0xFF000000u | ((uint32_t)r8 << 16) | ((uint32_t)g8 << 8) | b8;
}

/* ------------------------------------------------------------------ */
/*  Register write                                                      */
/* ------------------------------------------------------------------ */

void denise_write_reg(denise_t *d, uint16_t offset, uint16_t val)
{
    /* Bitplane control registers */
    switch (offset) {
    case 0x100: d->bplcon0 = val; return;
    case 0x102: d->bplcon1 = val; return;
    case 0x104: d->bplcon2 = val; return;
    default: break;
    }

    /*
     * Bitplane pointers: BPL1PTH/L at 0x0E0/0x0E2, BPL2PTH/L at 0x0E4/0x0E6,
     * …, BPL6PTH/L at 0x0F4/0x0F6.  Each pair is 4 bytes apart.
     *
     * plane index = (offset - 0x0E0) / 4
     * is_low_word = (offset - 0x0E0) & 2   (0 = high, 2 = low)
     *
     * The pointer is a 24-bit chip RAM address (Agnus has a 24-bit bus).
     * High word carries bits 20:16 in its low 5 bits.
     * Low word carries bits 15:1; bit 0 is always 0 (word-aligned).
     */
    if (offset >= 0x0E0 && offset <= 0x0F6) {
        unsigned idx    = (offset - 0x0E0) / 4;
        unsigned is_low = (offset - 0x0E0) & 2u;
        if (idx < DENISE_PLANES) {
            if (is_low)
                d->bplpt[idx] = (d->bplpt[idx] & 0x001F0000u) | (val & 0xFFFEu);
            else
                d->bplpt[idx] = (d->bplpt[idx] & 0x0000FFFFu) | ((uint32_t)(val & 0x1Fu) << 16);
        }
        return;
    }

    /* Palette: COLOR00–COLOR31 at 0x180–0x1BE (every 2 bytes) */
    if (offset >= 0x180 && offset <= 0x1BE) {
        unsigned idx = (offset - 0x180) / 2u;
        d->color[idx] = val & 0x0FFFu;   /* mask to 12 bits */
        return;
    }
}

/* ------------------------------------------------------------------ */
/*  Register read                                                       */
/* ------------------------------------------------------------------ */

uint16_t denise_read_reg(const denise_t *d, uint16_t offset)
{
    /* Colour registers are readable (useful for Copper MOVE verification) */
    if (offset >= 0x180 && offset <= 0x1BE)
        return d->color[(offset - 0x180) / 2u];

    return 0;
}

/* ------------------------------------------------------------------ */
/*  Pixel pipeline                                                      */
/* ------------------------------------------------------------------ */

void denise_render_line(denise_t *d,
                        const uint8_t *chip_ram, uint32_t chip_ram_size,
                        uint32_t *pixels)
{
    /*
     * Number of active bitplanes: BPLCON0 bits 14:12 (BPU field).
     * Valid range 0–6; clamp anything higher to 6.
     */
    int num_planes = (d->bplcon0 >> 12) & 7;
    if (num_planes > DENISE_PLANES)
        num_planes = DENISE_PLANES;

    for (int px = 0; px < DENISE_WIDTH; px++) {
        /*
         * Locate the bit for this pixel within each bitplane.
         *
         * Each bitplane is a sequence of 16-bit words stored MSB-first:
         *   word 0 holds pixels 0–15, word 1 holds pixels 16–31, …
         *
         * Within each word, pixel 0 is in bit 15 (the most significant bit)
         * and pixel 15 is in bit 0.  So:
         *   word index = px / 16
         *   bit inside word = 15 − (px % 16)
         */
        int word_idx = px / 16;
        int bit_pos  = 15 - (px % 16);

        uint8_t color_idx = 0;

        for (int n = 0; n < num_planes; n++) {
            /*
             * Each word is 2 bytes; the high byte comes first (big-endian,
             * matching the 68000's native byte order).
             */
            uint32_t addr = d->bplpt[n] + (uint32_t)(word_idx * 2);
            uint16_t word = 0;
            if (addr + 1 < chip_ram_size) {
                word = ((uint16_t)chip_ram[addr] << 8) | chip_ram[addr + 1];
            }

            /* Contribute this plane's bit to the colour index */
            if ((word >> bit_pos) & 1u)
                color_idx |= (uint8_t)(1u << n);
        }

        pixels[px] = denise_expand_color(d->color[color_idx]);
    }
}

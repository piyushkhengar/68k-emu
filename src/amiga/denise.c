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
/*  Mid-line color change tracking                                      */
/* ------------------------------------------------------------------ */

void denise_begin_scanline(denise_t *d)
{
    memcpy(d->pre_color, d->color, sizeof(d->pre_color));
    d->n_mid_changes = 0;
}

void denise_record_color_change(denise_t *d, uint16_t hpos,
                                uint8_t color_idx, uint16_t value)
{
    if (color_idx >= DENISE_COLORS) return;
    if (d->n_mid_changes >= DENISE_MAX_MID_CHANGES) return;
    /* Convert lores hpos to HIRES output pixel: pixel 0 corresponds to
     * hpos 0x81 (left edge of normal DIW). Negative trigger_x clamps to 0
     * so writes that happen "before the visible area" apply at line start. */
    int trigger_x = ((int)hpos - 0x81) * 2;
    if (trigger_x < 0) trigger_x = 0;
    if (trigger_x > DENISE_HIRES_W) trigger_x = DENISE_HIRES_W;
    int i = d->n_mid_changes;
    d->mid_changes[i].trigger_x = (uint16_t)trigger_x;
    d->mid_changes[i].color_idx = color_idx;
    d->mid_changes[i].value     = value;
    d->n_mid_changes = i + 1;
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

    /*
     * Sprite registers: SPRxPOS/CTL/DATA/DATB at 0x140–0x17E.
     * Each sprite occupies 4 consecutive word registers (8 bytes):
     *   offset = 0x140 + sprite_index * 8 + register_within_sprite * 2
     *   register_within_sprite: 0=POS 1=CTL 2=DATA 3=DATB
     */
    if (offset >= 0x140 && offset <= 0x17E) {
        unsigned rel = (offset - 0x140u) / 2u;   /* 0..31 */
        unsigned idx = rel / 4u;                  /* sprite index 0..7 */
        unsigned reg = rel % 4u;                  /* 0=pos 1=ctl 2=data 3=datb */
        if (idx < DENISE_SPRITES) {
            switch (reg) {
            case 0: d->spr[idx].pos  = val; break;
            case 1: d->spr[idx].ctl  = val; break;
            case 2: d->spr[idx].data = val; break;
            case 3: d->spr[idx].datb = val; break;
            }
        }
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
                        uint32_t *pixels,
                        int scroll_px, int diw_start, int diw_end)
{
    /*
     * Number of active bitplanes: BPLCON0 bits 14:12 (BPU field).
     * Valid range 0–6; clamp anything higher to 6.
     */
    int num_planes = (d->bplcon0 >> 12) & 7;
    if (num_planes > DENISE_PLANES)
        num_planes = DENISE_PLANES;

    int hires = (d->bplcon0 >> 15) & 1;

    /*
     * Effective per-pixel palette. Start from the snapshot taken at scanline
     * begin; mid_changes are replayed in order as out_x crosses each
     * trigger_x. If the caller never invoked denise_begin_scanline (older
     * test paths, paths that don't run Copper), n_mid_changes is 0 and
     * pre_color is whatever the last begin_scanline cached — fall back to
     * d->color in that case so behaviour matches the pre-mid-line code.
     */
    uint16_t cur_color[DENISE_COLORS];
    if (d->n_mid_changes > 0)
        memcpy(cur_color, d->pre_color, sizeof(cur_color));
    else
        memcpy(cur_color, d->color, sizeof(cur_color));
    int change_idx = 0;

    /* Iterate by OUTPUT pixel so we can apply mid-line color changes in
     * lock-step with x advancing. Source pixel = out_x - scroll_px. */
    if (hires) {
        for (int out_x = 0; out_x < DENISE_HIRES_W; out_x++) {
            while (change_idx < d->n_mid_changes &&
                   d->mid_changes[change_idx].trigger_x <= out_x) {
                cur_color[d->mid_changes[change_idx].color_idx] =
                    d->mid_changes[change_idx].value;
                change_idx++;
            }
            uint32_t bg = denise_expand_color(cur_color[0]);
            int px = out_x - scroll_px;
            if (out_x < diw_start || out_x >= diw_end ||
                px < 0 || px >= DENISE_HIRES_W) {
                pixels[out_x] = bg;
                continue;
            }

            int word_idx = px / 16;
            int bit_pos  = 15 - (px % 16);

            uint8_t color_idx = 0;
            for (int n = 0; n < num_planes; n++) {
                uint32_t addr = d->bplpt[n] + (uint32_t)(word_idx * 2);
                uint16_t word = 0;
                if (addr + 1 < chip_ram_size)
                    word = ((uint16_t)chip_ram[addr] << 8) | chip_ram[addr + 1];
                if ((word >> bit_pos) & 1u)
                    color_idx |= (uint8_t)(1u << n);
            }
            pixels[out_x] = denise_expand_color(cur_color[color_idx]);
        }
    } else {
        /* LORES: source pixel = (out_x - scroll_px) / 2 (each doubled). */
        for (int out_x = 0; out_x < DENISE_HIRES_W; out_x++) {
            while (change_idx < d->n_mid_changes &&
                   d->mid_changes[change_idx].trigger_x <= out_x) {
                cur_color[d->mid_changes[change_idx].color_idx] =
                    d->mid_changes[change_idx].value;
                change_idx++;
            }
            uint32_t bg = denise_expand_color(cur_color[0]);
            int px = (out_x - scroll_px) / 2;
            if (out_x < diw_start || out_x >= diw_end ||
                (out_x - scroll_px) < 0 || px >= DENISE_LORES_W) {
                pixels[out_x] = bg;
                continue;
            }

            int word_idx = px / 16;
            int bit_pos  = 15 - (px % 16);

            uint8_t color_idx = 0;
            for (int n = 0; n < num_planes; n++) {
                uint32_t addr = d->bplpt[n] + (uint32_t)(word_idx * 2);
                uint16_t word = 0;
                if (addr + 1 < chip_ram_size)
                    word = ((uint16_t)chip_ram[addr] << 8) | chip_ram[addr + 1];
                if ((word >> bit_pos) & 1u)
                    color_idx |= (uint8_t)(1u << n);
            }
            pixels[out_x] = denise_expand_color(cur_color[color_idx]);
        }
    }

    /*
     * Sprite overlay — rendered on top of bitplanes.
     *
     * Sprites 0–1 have highest priority, then 2–3, 4–5, 6–7.
     * We iterate highest priority last so lower-numbered sprites win.
     * (Iterating 7..0 means sprite 0 overwrites sprite 7 → sprite 0 wins.)
     *
     * Horizontal position: hstart = ((pos & 0xFF) << 1) | ((ctl >> 1) & 1).
     * The display window starts at hstart = 0x81 in LORES coordinates.
     * Output uses HIRES (640px): screen_x = (hstart - 0x81) * 2.
     *
     * A sprite is considered inactive when both DATA and DATB are zero
     * (the Copper writes 0/0 at vstop on real hardware).
     */
    for (int n = DENISE_SPRITES - 1; n >= 0; n--) {
        if (!d->spr[n].data && !d->spr[n].datb)
            continue;

        int hstart   = (int)(((d->spr[n].pos & 0xFFu) << 1) | ((d->spr[n].ctl >> 1) & 1u));
        int screen_x = (hstart - 0x81) * 2;   /* LORES → HIRES coords */
        int pair     = n / 2;
        int base     = 16 + pair * 4;   /* palette base for this sprite pair */

        for (int bit = 0; bit < 16; bit++) {
            /* Sprites are 16 LORES pixels wide → 32 HIRES pixels */
            int sx = screen_x + bit * 2;
            if (sx < 0 || sx + 1 >= DENISE_HIRES_W)
                continue;

            int bit0 = (d->spr[n].data >> (15 - bit)) & 1;
            int bit1 = (d->spr[n].datb >> (15 - bit)) & 1;
            int cidx = (bit1 << 1) | bit0;

            if (cidx != 0) {
                uint32_t c = denise_expand_color(d->color[base + cidx]);
                pixels[sx]     = c;
                pixels[sx + 1] = c;
            }
        }
    }
}

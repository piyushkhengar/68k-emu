/*
 * Denise — Amiga 500 video chip.
 *
 * Chapter 3 step 2 scope: colour registers, bitplane control, and the
 * pixel pipeline (bitplane fetch → colour index → 32-bit ARGB).
 * Sprites, HAM/EHB, and dual-playfield are deferred to later chapters.
 *
 * Register ownership (offsets from 0xDFF000 base):
 *
 *   Write-only:
 *     0x100  BPLCON0   Bitplane control: bits 14:12 = BPU (plane count 0–6)
 *     0x102  BPLCON1   Horizontal scroll (stored; ignored until Ch 7)
 *     0x104  BPLCON2   Sprite/playfield priority (stored; ignored until Ch 7)
 *     0x0E0  BPL1PTH   Bitplane 1 pointer high (bits 20:16)
 *     0x0E2  BPL1PTL   Bitplane 1 pointer low  (bits 15:1, bit 0 always 0)
 *     … (BPL2–BPL6 at 0x0E4–0x0F4 in steps of 4)
 *     0x180  COLOR00 – 0x1BE COLOR31   Palette (12-bit 0x0RGB, write)
 *
 *   Read (emulator convenience; write-only on real hardware for most):
 *     0x180–0x1BE  COLOR00–31 readable for Copper MOVE verification
 *
 * Pixel pipeline (denise_render_line):
 *   For each pixel 0–319:
 *     1. word index  = px / 16
 *     2. bit position = 15 − (px % 16)        ← MSB is the leftmost pixel
 *     3. For each active plane n: sample bit from chip_ram[bplpt[n] + word*2]
 *     4. Assemble colour index (plane 0 → bit 0, plane 1 → bit 1, …)
 *     5. Look up color[idx], expand 0x0RGB → 0xFFRRGGBB
 *
 * Colour expansion (denise_expand_color):
 *   Each 4-bit channel nibble is replicated: 0xF → 0xFF, 0x8 → 0x88.
 *   This matches the Amiga DAC behaviour (no linear scaling).
 */

#ifndef AMIGA_DENISE_H
#define AMIGA_DENISE_H

#include <stdint.h>

#define DENISE_WIDTH   320   /* lores pixel width */
#define DENISE_PLANES    6   /* maximum bitplanes */
#define DENISE_COLORS   32   /* palette entries   */

typedef struct {
    uint16_t color[DENISE_COLORS];  /* palette: 12-bit 0x0RGB per entry */
    uint16_t bplcon0;               /* bitplane control register 0       */
    uint16_t bplcon1;               /* horizontal scroll (stored only)   */
    uint16_t bplcon2;               /* priority control (stored only)    */
    uint32_t bplpt[DENISE_PLANES];  /* bitplane pointers into chip RAM   */
} denise_t;

/* Lifecycle */
void denise_init(denise_t *d);
void denise_reset(denise_t *d);

/*
 * Register access (custom chip window, offset from 0xDFF000).
 * Only Denise-owned offsets are handled; all others return 0 / are ignored.
 */
void     denise_write_reg(denise_t *d, uint16_t offset, uint16_t val);
uint16_t denise_read_reg(const denise_t *d, uint16_t offset);

/*
 * denise_expand_color — convert a 12-bit Amiga colour to 32-bit ARGB.
 *
 * Input:  0x0RGB  (4 bits each, upper nibble must be 0)
 * Output: 0xFFRRGGBB  (alpha = 0xFF, each channel = nibble replicated)
 *
 * Example: 0x0F80 → R=0xFF, G=0x88, B=0x00 → 0xFFFF8800
 */
uint32_t denise_expand_color(uint16_t rgb12);

/*
 * denise_render_line — rasterise one 320-pixel lores scanline.
 *
 * Reads bitplane data from chip_ram[] at the addresses stored in d->bplpt[].
 * Writes 320 ARGB8888 pixels to the pixels[] array.
 *
 * The caller (Agnus) is responsible for advancing bplpt[] between scanlines.
 * chip_ram_size is used for bounds checking; out-of-range reads yield 0.
 */
void denise_render_line(denise_t *d,
                        const uint8_t *chip_ram, uint32_t chip_ram_size,
                        uint32_t *pixels);

#endif /* AMIGA_DENISE_H */

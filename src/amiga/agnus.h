/*
 * Agnus — Amiga 500 DMA controller, Copper, Blitter, and beam counter.
 *
 * Chapter 3 step 1 scope: DMA control register + PAL beam counters.
 * Copper, Blitter, and bitplane DMA are added in later steps.
 *
 * Register ownership (offsets from 0xDFF000 base):
 *   Read:
 *     0x002  DMACONR   DMA control register (read mirror)
 *     0x004  VPOSR     Beam counter high: bit 0 = line[8], bits 15:1 = 0
 *     0x006  VHPOSR    Beam counter low:  bits 15:8 = line[7:0],
 *                                         bits  7:0 = hpos[7:0]
 *   Write (SET/CLR: bit 15 = 1 → set lower 15 bits, 0 → clear):
 *     0x096  DMACON    DMA enable flags
 *
 * DMACON bit constants (Agnus-owned subset):
 *   Bit 9  DMAEN — master DMA enable (must be set for any DMA to run)
 *   Bit 8  BPLEN — bitplane DMA enable
 *   Bit 7  COPEN — Copper DMA enable
 *   Bit 6  BLTEN — Blitter DMA enable
 *   Bit 5  SPREN — sprite DMA enable
 *   Bit 4  DSKEN — disk DMA enable
 *   Bits 3:0 — audio channels 3:0 (Paula-owned; Agnus mirrors them in DMACONR)
 *
 * PAL beam:
 *   312 scanlines per frame, 454 CPU cycles per scanline.
 *   VPOSR bit 0 = line >> 8   (1 only for lines 256-311)
 *   VHPOSR bits 15:8 = line & 0xFF
 *   VHPOSR bits 7:0  = hpos (horizontal position; used sub-scanline from Ch 8)
 *
 * Kickstart reads VPOSR/VHPOSR in a tight loop to detect vblank
 * (waiting for the beam to leave the active area).  Correct values here
 * are required for the headless boot to advance past the first few frames.
 */

#ifndef AMIGA_AGNUS_H
#define AMIGA_AGNUS_H

#include <stdint.h>

/* DMACON bits (Agnus-owned) */
#define DMACON_DMAEN  (1u << 9)   /* master DMA enable */
#define DMACON_BPLEN  (1u << 8)   /* bitplane DMA */
#define DMACON_COPEN  (1u << 7)   /* Copper DMA */
#define DMACON_BLTEN  (1u << 6)   /* Blitter DMA */
#define DMACON_SPREN  (1u << 5)   /* sprite DMA */
#define DMACON_DSKEN  (1u << 4)   /* disk DMA */

typedef struct {
    uint16_t dmacon;  /* accumulated DMA control state (all bits inc. Paula's) */
    int      line;    /* current PAL scanline 0–311                            */
    int      hpos;    /* horizontal beam position 0–453; updated per color clock*/
} agnus_t;

/* Lifecycle */
void agnus_init(agnus_t *ag);
void agnus_reset(agnus_t *ag);

/*
 * agnus_tick_scanline — advance beam to the start of the given scanline.
 *
 * Call once per scanline from the main run loop, before cpu_step() loops.
 * Sets ag->line = line and resets ag->hpos = 0.
 */
void agnus_tick_scanline(agnus_t *ag, int line);

/*
 * Register read/write (custom chip window, offset from 0xDFF000).
 * Only Agnus-owned offsets are handled; all others return 0 / are ignored.
 */
uint16_t agnus_read_reg(const agnus_t *ag, uint16_t offset);
void     agnus_write_reg(agnus_t *ag, uint16_t offset, uint16_t val);

#endif /* AMIGA_AGNUS_H */

/*
 * Paula — Amiga 500 audio/disk/UART chip.
 *
 * Phase 2 scope: 4-channel audio register model + direct-mode tick.
 * DMA fetch from chip RAM is deferred to Phase 3.
 *
 * Custom chip register offsets (all relative to 0xDFF000):
 *
 *   Read-only:
 *     0x002  DMACONR   DMA control bits (read)
 *     0x010  ADKCONR   Audio/disk key control (read)
 *     0x01C  INTENAR   Interrupt enable flags (read)
 *     0x01E  INTREQR   Interrupt request flags (read)
 *
 *   Write-only (SET/CLR: bit 15 = 1 → set lower 15 bits,
 *                        bit 15 = 0 → clear lower 15 bits):
 *     0x096  DMACON    DMA control (bit 9 = audio master, bits 3:0 = ch 3:0)
 *     0x09A  INTENA    Interrupt enable
 *     0x09C  INTREQ    Interrupt request
 *     0x09E  ADKCON    Audio/disk key
 *
 *   Per-channel n (n = 0..3); channel base = 0x0A0 + n * 0x10:
 *     base+0x0  AUDnLCH  DMA pointer high 3 bits (written as 16-bit word)
 *     base+0x2  AUDnLCL  DMA pointer low 16 bits
 *     base+0x4  AUDnLEN  Word count for DMA block
 *     base+0x6  AUDnPER  Period in color clocks (0 = channel silent)
 *     base+0x8  AUDnVOL  Volume 0–64; values above 64 are clamped to 64
 *     base+0xA  AUDnDAT  Direct sample: high byte = sample, loads period counter
 *
 * Stereo routing (hardware fixed):
 *   Left  output = ch[0] + ch[3]
 *   Right output = ch[1] + ch[2]
 *
 * Period counter:
 *   Writing AUDnDAT loads ch.sample from the high byte of val and resets
 *   per_cnt to ch.per.  Each color clock in paula_tick() decrements per_cnt;
 *   when per_cnt reaches 0 the channel fires (out_sample = sample * vol / 64)
 *   and per_cnt reloads from per.  per = 0 silences the channel.
 *
 * INTREQ bit constants (used with paula_assert_intreq):
 *   These are the hardware INTREQ bit positions (0xDFF09E).
 *   The CPU interrupt levels they map to:
 *     Level 1: TBE (bit 0), DSKBLK (bit 1), SOFT (bit 2)
 *     Level 2: PORTS / CIA-A (bit 3)
 *     Level 3: COPER (bit 4), VERTB (bit 5), BLIT (bit 6)
 *     Level 4: AUD0-3 (bits 7-10)
 *     Level 5: RBF (bit 11), DSKSYN (bit 12)
 *     Level 6: EXTER / CIA-B (bit 13)
 */

#ifndef AMIGA_PAULA_H
#define AMIGA_PAULA_H

#include <stdint.h>
#include <stdbool.h>

/* ------------------------------------------------------------------ */
/*  INTREQ bit constants (hardware INTREQ register, 0xDFF09E)          */
/* ------------------------------------------------------------------ */

#define INTREQ_TBE    (1u << 0)   /* Serial transmit buffer empty  — level 1 */
#define INTREQ_DSKBLK (1u << 1)   /* Disk block DMA done           — level 1 */
#define INTREQ_SOFT   (1u << 2)   /* Software interrupt            — level 1 */
#define INTREQ_PORTS  (1u << 3)   /* CIA-A / I/O ports             — level 2 */
#define INTREQ_COPER  (1u << 4)   /* Copper                        — level 3 */
#define INTREQ_VERTB  (1u << 5)   /* Vertical blank                — level 3 */
#define INTREQ_BLIT   (1u << 6)   /* Blitter finished              — level 3 */
#define INTREQ_AUD0   (1u << 7)   /* Audio channel 0               — level 4 */
#define INTREQ_AUD1   (1u << 8)   /* Audio channel 1               — level 4 */
#define INTREQ_AUD2   (1u << 9)   /* Audio channel 2               — level 4 */
#define INTREQ_AUD3   (1u << 10)  /* Audio channel 3               — level 4 */
#define INTREQ_RBF    (1u << 11)  /* Serial receive buffer full    — level 5 */
#define INTREQ_DSKSYN (1u << 12)  /* Disk sync match               — level 5 */
#define INTREQ_EXTER  (1u << 13)  /* CIA-B / external              — level 6 */

/* ------------------------------------------------------------------ */
/*  Data types                                                          */
/* ------------------------------------------------------------------ */

typedef struct {
    /* Registers (written by paula_write_reg) */
    uint32_t lc;        /* DMA location: (LCH[2:0] << 16) | LCL */
    uint16_t len;       /* DMA word count */
    uint16_t per;       /* period in color clocks; 0 = silent */
    uint8_t  vol;       /* volume 0–64 (clamped on write) */
    int8_t   sample;    /* current PCM sample byte (from AUDnDAT high byte) */

    /* Internal state */
    uint16_t per_cnt;   /* countdown; loaded from per on AUDnDAT write */
    int16_t  out_sample;/* last committed DAC output (sample * vol / 64) */
} paula_channel_t;

typedef struct {
    paula_channel_t ch[4];
    uint16_t dmacon;    /* latched DMACON register state */
    uint16_t adkcon;    /* latched ADKCON register state */
    uint16_t intena;    /* latched INTENA register state */
    uint16_t intreq;    /* latched INTREQ register state */
} paula_t;

/* ------------------------------------------------------------------ */
/*  API                                                                 */
/* ------------------------------------------------------------------ */

/* Initialise all fields to zero. */
void paula_init(paula_t *p);

/* Reset all fields to zero (same effect as init for Phase 2). */
void paula_reset(paula_t *p);

/*
 * Write a custom-chip register.
 * offset: address relative to 0xDFF000.
 * val:    16-bit value to write.
 * SET/CLR registers (DMACON, INTENA, INTREQ, ADKCON): bit 15 of val
 * selects set (1) or clear (0) mode for the lower 15 bits.
 */
void paula_write_reg(paula_t *p, uint16_t offset, uint16_t val);

/*
 * Read a custom-chip register.
 * Returns the current value of the readable register at offset.
 * Write-only registers (AUDnXXX, DMACON, INTENA, INTREQ, ADKCON) return 0.
 */
uint16_t paula_read_reg(const paula_t *p, uint16_t offset);

/*
 * Assert bits in the hardware INTREQ register and immediately re-evaluate
 * the interrupt priority level sent to the CPU.
 *
 * bits: one or more INTREQ_xxx constants OR'd together.
 *
 * NOTE on field naming: due to an offset mapping in this codebase, the
 * hardware INTREQ register (write at 0xDFF09E) is stored in p->adkcon, and
 * the hardware INTENA register (write at 0xDFF09C) is stored in p->intreq.
 * This naming is confusing but consistent throughout paula.c and paula_tests.c.
 * paula_assert_intreq() handles this correctly internally.
 */
void paula_assert_intreq(paula_t *p, uint16_t bits);

/*
 * Re-evaluate interrupt priority: AND the enable register (p->intreq) with
 * the request register (p->adkcon), derive the highest pending level (0-6),
 * and write it to cpu_ipl.  Called automatically by paula_assert_intreq().
 */
void paula_update_irq(paula_t *p);

/*
 * Advance Paula by color_clocks Amiga color clocks (~280 ns each).
 *
 * Computes one stereo output pair:
 *   *out_left  = ch[0].out_sample + ch[3].out_sample
 *   *out_right = ch[1].out_sample + ch[2].out_sample
 *
 * In direct mode (channel DMA off) a channel repeats its AUDnDAT sample
 * at the rate determined by per.  out_sample is updated on period fires
 * and held between fires.  out_sample = 0 before the first fire.
 */
void paula_tick(paula_t *p, uint32_t color_clocks,
                int16_t *out_left, int16_t *out_right);

#endif /* AMIGA_PAULA_H */

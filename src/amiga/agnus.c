/*
 * Agnus — Chapter 3 step 1: DMA control register + PAL beam counters.
 *
 * This file deliberately contains only the minimal subset needed to make
 * VPOSR/VHPOSR readable and DMACON writable.  The Copper, Blitter, and
 * bitplane DMA engines are added in later steps.
 */

#include "agnus.h"
#include <string.h>

/* ------------------------------------------------------------------ */
/*  Lifecycle                                                           */
/* ------------------------------------------------------------------ */

void agnus_init(agnus_t *ag)
{
    memset(ag, 0, sizeof(*ag));
}

void agnus_reset(agnus_t *ag)
{
    memset(ag, 0, sizeof(*ag));
}

/* ------------------------------------------------------------------ */
/*  Beam counter                                                        */
/* ------------------------------------------------------------------ */

void agnus_tick_scanline(agnus_t *ag, int line)
{
    ag->line = line;
    ag->hpos = 0;   /* hpos resets to 0 at the start of every scanline */
}

/* ------------------------------------------------------------------ */
/*  Register access                                                     */
/* ------------------------------------------------------------------ */

uint16_t agnus_read_reg(const agnus_t *ag, uint16_t offset)
{
    switch (offset) {
    case 0x002:  /* DMACONR — read mirror of DMACON */
        return ag->dmacon;

    case 0x004:  /* VPOSR — bit 0 = bit 8 of the scanline counter */
        /*
         * The real Agnus encodes the 9-bit vertical beam position
         * across two registers:
         *   VPOSR  bit 0 = line[8]   (1 only for lines 256-311)
         *   VHPOSR bits 15:8 = line[7:0]
         * Kickstart polls VPOSR to detect long-frame vs short-frame (PAL/NTSC).
         */
        return (uint16_t)((ag->line >> 8) & 1u);

    case 0x006:  /* VHPOSR — vertical low byte in high byte, hpos in low byte */
        return (uint16_t)(((ag->line & 0xFFu) << 8) | (ag->hpos & 0xFFu));

    default:
        return 0;
    }
}

void agnus_write_reg(agnus_t *ag, uint16_t offset, uint16_t val)
{
    switch (offset) {
    case 0x096:  /* DMACON — SET/CLR register */
        /*
         * Bit 15 = 1 → set the bits in [14:0].
         * Bit 15 = 0 → clear the bits in [14:0].
         * This is the same convention used by Paula's INTENA/INTREQ.
         */
        if (val & 0x8000u)
            ag->dmacon |=  (uint16_t)(val & 0x7FFFu);
        else
            ag->dmacon &= (uint16_t)~(val & 0x7FFFu);
        break;

    default:
        break;
    }
}

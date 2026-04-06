/*
 * Unit tests for src/amiga/agnus.c — Chapter 3 step 1.
 *
 * Tests are written directly against the agnus_t struct; no bus routing,
 * no CPU execution.  Each test function creates its own agnus_t on the
 * stack so tests are completely isolated from one another.
 *
 * Coverage:
 *   - Init state (all fields zero)
 *   - agnus_tick_scanline: line stored, hpos reset
 *   - VPOSR encoding for lines 0, 100, 255, 256, 311
 *   - VHPOSR encoding for the same set of lines
 *   - DMACONR read mirrors the accumulated dmacon state
 *   - DMACON SET: bit 15 = 1 ORs bits into dmacon
 *   - DMACON CLR: bit 15 = 0 ANDs-NOT bits out of dmacon
 *   - Unknown register read returns 0, write is silently ignored
 */

#include "agnus.h"
#include "agnus_tests.h"
#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/*  Minimal test framework (mirrors cia_tests.c convention)            */
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
/*  Helper                                                              */
/* ------------------------------------------------------------------ */

static agnus_t ag;

static void reset_agnus(void)
{
    agnus_init(&ag);
}

/* ------------------------------------------------------------------ */
/*  Test: init state                                                    */
/* ------------------------------------------------------------------ */

static void test_init_state(void)
{
    reset_agnus();
    BASSERT(ag.dmacon == 0, "dmacon should be 0 after init");
    BASSERT(ag.line   == 0, "line should be 0 after init");
    BASSERT(ag.hpos   == 0, "hpos should be 0 after init");
}

/* ------------------------------------------------------------------ */
/*  Test: agnus_tick_scanline stores the line and resets hpos          */
/* ------------------------------------------------------------------ */

static void test_tick_scanline_stores_line(void)
{
    reset_agnus();

    agnus_tick_scanline(&ag, 0);
    BASSERT(ag.line == 0, "line should be 0");
    BASSERT(ag.hpos == 0, "hpos should reset to 0");

    agnus_tick_scanline(&ag, 100);
    BASSERT(ag.line == 100, "line should be 100");
    BASSERT(ag.hpos == 0,   "hpos should reset to 0 on each scanline");

    agnus_tick_scanline(&ag, 311);
    BASSERT(ag.line == 311, "line should be 311 (last PAL line)");
}

/* ------------------------------------------------------------------ */
/*  Test: VPOSR — bit 0 encodes line[8]                               */
/* ------------------------------------------------------------------ */

static void test_vposr_beam_high_bit(void)
{
    reset_agnus();

    /* Lines 0-255: bit 8 = 0, so VPOSR should be 0. */
    agnus_tick_scanline(&ag, 0);
    BASSERT(agnus_read_reg(&ag, 0x004) == 0,
            "VPOSR should be 0 for line 0");

    agnus_tick_scanline(&ag, 255);
    BASSERT(agnus_read_reg(&ag, 0x004) == 0,
            "VPOSR should be 0 for line 255 (bit 8 still 0)");

    /* Line 256: bit 8 becomes 1, so VPOSR bit 0 = 1. */
    agnus_tick_scanline(&ag, 256);
    BASSERT(agnus_read_reg(&ag, 0x004) == 1,
            "VPOSR should be 1 for line 256 (bit 8 = 1)");

    agnus_tick_scanline(&ag, 311);
    BASSERT(agnus_read_reg(&ag, 0x004) == 1,
            "VPOSR should be 1 for line 311");
}

/* ------------------------------------------------------------------ */
/*  Test: VHPOSR — bits 15:8 encode line[7:0], bits 7:0 encode hpos   */
/* ------------------------------------------------------------------ */

static void test_vhposr_beam_low_byte(void)
{
    reset_agnus();

    /* Line 0: VHPOSR = 0x0000 */
    agnus_tick_scanline(&ag, 0);
    BASSERT(agnus_read_reg(&ag, 0x006) == 0x0000,
            "VHPOSR should be 0x0000 for line 0, hpos 0");

    /* Line 100 = 0x64: VHPOSR bits 15:8 = 0x64, bits 7:0 = 0 */
    agnus_tick_scanline(&ag, 100);
    BASSERT(agnus_read_reg(&ag, 0x006) == 0x6400,
            "VHPOSR should be 0x6400 for line 100");

    /* Line 256 = 0x100: VHPOSR bits 15:8 = 0x00 (low byte of 256), bits 7:0 = 0 */
    agnus_tick_scanline(&ag, 256);
    BASSERT(agnus_read_reg(&ag, 0x006) == 0x0000,
            "VHPOSR should be 0x0000 for line 256 (low byte = 0)");

    /* Line 311 = 0x137: VHPOSR bits 15:8 = 0x37, bits 7:0 = 0 */
    agnus_tick_scanline(&ag, 311);
    BASSERT(agnus_read_reg(&ag, 0x006) == 0x3700,
            "VHPOSR should be 0x3700 for line 311");
}

/* ------------------------------------------------------------------ */
/*  Test: VPOSR + VHPOSR together reconstruct the full line number     */
/* ------------------------------------------------------------------ */

static void test_beam_reconstruction(void)
{
    reset_agnus();

    /*
     * The hardware encodes a 9-bit line counter split across two registers:
     *   full_line = (VPOSR & 1) << 8 | (VHPOSR >> 8)
     * Verify reconstruction works for lines above and below 256.
     */
    int test_lines[] = { 0, 1, 127, 128, 255, 256, 257, 311 };
    int n = (int)(sizeof(test_lines) / sizeof(test_lines[0]));

    for (int i = 0; i < n; i++) {
        int expected = test_lines[i];
        agnus_tick_scanline(&ag, expected);
        uint16_t vposr  = agnus_read_reg(&ag, 0x004);
        uint16_t vhposr = agnus_read_reg(&ag, 0x006);
        int reconstructed = ((vposr & 1) << 8) | (vhposr >> 8);
        BASSERT(reconstructed == expected,
                "line %d: reconstructed=%d from VPOSR=0x%04X VHPOSR=0x%04X",
                expected, reconstructed, vposr, vhposr);
    }
}

/* ------------------------------------------------------------------ */
/*  Test: DMACONR reads back accumulated DMACON state                  */
/* ------------------------------------------------------------------ */

static void test_dmaconr_read(void)
{
    reset_agnus();

    /* Fresh state: all DMA off */
    BASSERT(agnus_read_reg(&ag, 0x002) == 0,
            "DMACONR should be 0 after init");

    /* Set master DMA enable (bit 9) */
    agnus_write_reg(&ag, 0x096, 0x8200);   /* 0x8000 | DMACON_DMAEN */
    BASSERT(agnus_read_reg(&ag, 0x002) == DMACON_DMAEN,
            "DMACONR should reflect DMACON_DMAEN after SET write");
}

/* ------------------------------------------------------------------ */
/*  Test: DMACON SET ORs bits in                                       */
/* ------------------------------------------------------------------ */

static void test_dmacon_set(void)
{
    reset_agnus();

    /* Set DMAEN (bit 9) and BPLEN (bit 8) together */
    agnus_write_reg(&ag, 0x096, (uint16_t)(0x8000u | DMACON_DMAEN | DMACON_BPLEN));
    BASSERT((ag.dmacon & DMACON_DMAEN) != 0, "DMAEN should be set");
    BASSERT((ag.dmacon & DMACON_BPLEN) != 0, "BPLEN should be set");
    BASSERT((ag.dmacon & DMACON_COPEN) == 0, "COPEN should still be clear");

    /* Set COPEN on top, leaving DMAEN/BPLEN intact */
    agnus_write_reg(&ag, 0x096, (uint16_t)(0x8000u | DMACON_COPEN));
    BASSERT((ag.dmacon & DMACON_DMAEN) != 0, "DMAEN should still be set");
    BASSERT((ag.dmacon & DMACON_BPLEN) != 0, "BPLEN should still be set");
    BASSERT((ag.dmacon & DMACON_COPEN) != 0, "COPEN should now be set");
}

/* ------------------------------------------------------------------ */
/*  Test: DMACON CLR ANDs bits out                                     */
/* ------------------------------------------------------------------ */

static void test_dmacon_clr(void)
{
    reset_agnus();

    /* Set DMAEN + BPLEN + COPEN */
    agnus_write_reg(&ag, 0x096,
        (uint16_t)(0x8000u | DMACON_DMAEN | DMACON_BPLEN | DMACON_COPEN));

    /* Clear just BPLEN (bit 15 = 0) */
    agnus_write_reg(&ag, 0x096, (uint16_t)DMACON_BPLEN);
    BASSERT((ag.dmacon & DMACON_DMAEN) != 0, "DMAEN should still be set");
    BASSERT((ag.dmacon & DMACON_BPLEN) == 0, "BPLEN should be cleared");
    BASSERT((ag.dmacon & DMACON_COPEN) != 0, "COPEN should still be set");

    /* Clear all */
    agnus_write_reg(&ag, 0x096,
        (uint16_t)(DMACON_DMAEN | DMACON_BPLEN | DMACON_COPEN));
    BASSERT(ag.dmacon == 0, "dmacon should be 0 after clearing all bits");
}

/* ------------------------------------------------------------------ */
/*  Test: unknown register reads return 0; writes are silently ignored */
/* ------------------------------------------------------------------ */

static void test_unknown_register(void)
{
    reset_agnus();
    ag.dmacon = 0x01FF;   /* set known state */

    /* Read of an unowned offset */
    uint16_t val = agnus_read_reg(&ag, 0x080);
    BASSERT(val == 0, "unknown register read should return 0");

    /* Write to unowned offset should not corrupt dmacon */
    agnus_write_reg(&ag, 0x080, 0xDEAD);
    BASSERT(ag.dmacon == 0x01FF,
            "unknown register write should not corrupt dmacon");
}

/* ------------------------------------------------------------------ */
/*  Public entry point                                                  */
/* ------------------------------------------------------------------ */

int run_agnus_tests(void)
{
    failures = 0;
    total    = 0;

    printf("  [agnus] beam counters + DMACON\n");

    test_init_state();
    test_tick_scanline_stores_line();
    test_vposr_beam_high_bit();
    test_vhposr_beam_low_byte();
    test_beam_reconstruction();
    test_dmaconr_read();
    test_dmacon_set();
    test_dmacon_clr();
    test_unknown_register();

    printf("  [agnus] %d/%d passed\n", total - failures, total);
    return failures > 0 ? 1 : 0;
}

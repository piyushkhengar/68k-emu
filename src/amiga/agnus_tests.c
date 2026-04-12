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

    /*
     * DMACONR bit 14 = BLTDONE (1 = blitter idle/done).
     * In our synchronous model the blitter is always done, so bit 14
     * is always set regardless of the dmacon state.
     */
    BASSERT(agnus_read_reg(&ag, 0x002) == 0x4000u,
            "DMACONR should be 0x4000 (BLTDONE set) after init");

    /* Set master DMA enable (bit 9) */
    agnus_write_reg(&ag, 0x096, 0x8200);   /* 0x8000 | DMACON_DMAEN */
    BASSERT(agnus_read_reg(&ag, 0x002) == (DMACON_DMAEN | 0x4000u),
            "DMACONR should reflect DMACON_DMAEN | BLTDONE after SET write");
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
/*  Copper tests                                                        */
/* ------------------------------------------------------------------ */

/* Capture up to 32 write_reg calls from the Copper for inspection */
static struct { uint16_t off; uint16_t val; } copper_writes[32];
static int copper_write_count;

static void capture_write(uint16_t off, uint16_t val)
{
    if (copper_write_count < 32) {
        copper_writes[copper_write_count].off = off;
        copper_writes[copper_write_count].val = val;
        copper_write_count++;
    }
}

/* Helper: fill fake_ram with a 2-word copper instruction */
static uint8_t copper_ram[256];

static void copper_poke(int addr, uint16_t ir1, uint16_t ir2)
{
    copper_ram[addr + 0] = (uint8_t)(ir1 >> 8);
    copper_ram[addr + 1] = (uint8_t)(ir1 & 0xFF);
    copper_ram[addr + 2] = (uint8_t)(ir2 >> 8);
    copper_ram[addr + 3] = (uint8_t)(ir2 & 0xFF);
}

static void test_copper_move(void)
{
    reset_agnus();
    memset(copper_ram, 0, sizeof(copper_ram));
    copper_write_count = 0;

    /* Enable DMAEN + COPEN */
    agnus_write_reg(&ag, 0x096, (uint16_t)(0x8000u | DMACON_DMAEN | DMACON_COPEN));

    /*
     * Copper list at address 0:
     *   MOVE COLOR00 (0x180) = 0x0AAA   -> IR1 = 0x0180 (bit 0 = 0), IR2 = 0x0AAA
     *   end sentinel
     */
    copper_poke(0,  0x0180, 0x0AAA);
    copper_poke(4,  0xFFFF, 0xFFFE);  /* end sentinel */

    ag.copper_pc = 0;
    agnus_tick_scanline(&ag, 0);
    agnus_copper_scanline(&ag, copper_ram, sizeof(copper_ram), capture_write);

    BASSERT(copper_write_count == 1,
            "MOVE: expected 1 write, got %d", copper_write_count);
    BASSERT(copper_writes[0].off == 0x0180,
            "MOVE: expected offset 0x0180, got 0x%04X", copper_writes[0].off);
    BASSERT(copper_writes[0].val == 0x0AAA,
            "MOVE: expected value 0x0AAA, got 0x%04X", copper_writes[0].val);
}

static void test_copper_wait_future_line(void)
{
    reset_agnus();
    memset(copper_ram, 0, sizeof(copper_ram));
    copper_write_count = 0;

    agnus_write_reg(&ag, 0x096, (uint16_t)(0x8000u | DMACON_DMAEN | DMACON_COPEN));

    /*
     * Copper list:
     *   WAIT for line 10 (VP=10, HP=0, BFV=0xFF, BFH=0xFE):
     *     IR1 = (10<<8)|0x01 = 0x0A01  (bit 0 = 1 = WAIT)
     *     IR2 = 0xFFFE               (BFV=0xFF, BFH=0xFE, bit 0 = 0 = WAIT)
     *   MOVE COLOR00 = 0x0AAA  (should NOT execute — wait not satisfied)
     */
    copper_poke(0, 0x0A01, 0xFFFE);
    copper_poke(4, 0x0180, 0x0AAA);
    copper_poke(8, 0xFFFF, 0xFFFE);

    ag.copper_pc = 0;
    agnus_tick_scanline(&ag, 5);   /* line 5 < wait target 10 */
    agnus_copper_scanline(&ag, copper_ram, sizeof(copper_ram), capture_write);

    BASSERT(copper_write_count == 0,
            "WAIT future: should not execute MOVE on line 5");
    /* copper_pc should have rewound to the WAIT instruction */
    BASSERT(ag.copper_pc == 0,
            "WAIT future: copper_pc should rewind to WAIT, got %u", ag.copper_pc);
}

static void test_copper_wait_past_line(void)
{
    reset_agnus();
    memset(copper_ram, 0, sizeof(copper_ram));
    copper_write_count = 0;

    agnus_write_reg(&ag, 0x096, (uint16_t)(0x8000u | DMACON_DMAEN | DMACON_COPEN));

    /* Same list but we're already past line 10 */
    copper_poke(0, 0x0A01, 0xFFFE);
    copper_poke(4, 0x0180, 0x0AAA);
    copper_poke(8, 0xFFFF, 0xFFFE);

    ag.copper_pc = 0;
    agnus_tick_scanline(&ag, 15);  /* line 15 > wait target 10 */
    agnus_copper_scanline(&ag, copper_ram, sizeof(copper_ram), capture_write);

    BASSERT(copper_write_count == 1,
            "WAIT past: should execute MOVE, got %d writes", copper_write_count);
    BASSERT(copper_writes[0].off == 0x0180 && copper_writes[0].val == 0x0AAA,
            "WAIT past: wrong write off=0x%04X val=0x%04X",
            copper_writes[0].off, copper_writes[0].val);
}

static void test_copper_dma_disabled(void)
{
    reset_agnus();
    memset(copper_ram, 0, sizeof(copper_ram));
    copper_write_count = 0;

    /* DMACON = 0: no DMA enabled */
    copper_poke(0, 0x0180, 0x0ABC);
    copper_poke(4, 0xFFFF, 0xFFFE);

    ag.copper_pc = 0;
    agnus_tick_scanline(&ag, 0);
    agnus_copper_scanline(&ag, copper_ram, sizeof(copper_ram), capture_write);

    BASSERT(copper_write_count == 0,
            "DMA disabled: Copper should not run");
}

/* ------------------------------------------------------------------ */
/*  Blitter tests                                                       */
/* ------------------------------------------------------------------ */

#define BLT_RAM_SIZE 4096u
static uint8_t blt_ram[BLT_RAM_SIZE];

/* Helper: read a 16-bit big-endian word from blt_ram */
static uint16_t blt_read(uint32_t addr)
{
    return (uint16_t)((blt_ram[addr] << 8) | blt_ram[addr + 1]);
}

static void test_blitter_zerofill(void)
{
    reset_agnus();
    memset(blt_ram, 0xFF, sizeof(blt_ram));

    /*
     * Zero-fill 4 words at address 0x100.
     * Minterm 0x00 → D is always 0 regardless of inputs.
     * USED only; USEA/B/C = 0.
     * Height = 1, width = 4 words.
     */
    ag.bltcon0  = (uint16_t)(0x0100u);  /* USED=1, minterm=0x00 */
    ag.bltcon1  = 0;
    ag.bltdpt   = 0x100;
    ag.bltdmod  = 0;
    ag.bltafwm  = 0xFFFF;
    ag.bltalwm  = 0xFFFF;

    /* BLTSIZE: height=1, width=4 → (1<<6)|4 = 0x0044 */
    agnus_blitter_execute(&ag, blt_ram, BLT_RAM_SIZE, (uint16_t)((1u << 6) | 4u));

    for (int i = 0; i < 4; i++) {
        BASSERT(blt_read((uint32_t)(0x100 + i * 2)) == 0,
                "zero-fill word %d: expected 0x0000, got 0x%04X",
                i, blt_read((uint32_t)(0x100 + i * 2)));
    }
}

static void test_blitter_copy(void)
{
    reset_agnus();
    memset(blt_ram, 0, sizeof(blt_ram));

    /*
     * Copy source A (4 words at 0x200) to destination D (at 0x300).
     * Minterm 0xF0 → D = A (when USEC/USEB=0, only A matters).
     * USEA=1, USED=1.
     */
    ag.bltcon0  = (uint16_t)(0x0B00u | 0xF0u); /* USEA|USED, minterm 0xF0 */
    ag.bltcon1  = 0;
    ag.bltapt   = 0x200;
    ag.bltdpt   = 0x300;
    ag.bltamod  = 0;
    ag.bltdmod  = 0;
    ag.bltafwm  = 0xFFFF;
    ag.bltalwm  = 0xFFFF;

    /* Source: 0xAAAA, 0x5555, 0x1234, 0xDEAD */
    blt_ram[0x200] = 0xAA; blt_ram[0x201] = 0xAA;
    blt_ram[0x202] = 0x55; blt_ram[0x203] = 0x55;
    blt_ram[0x204] = 0x12; blt_ram[0x205] = 0x34;
    blt_ram[0x206] = 0xDE; blt_ram[0x207] = 0xAD;

    agnus_blitter_execute(&ag, blt_ram, BLT_RAM_SIZE, (uint16_t)((1u << 6) | 4u));

    BASSERT(blt_read(0x300) == 0xAAAA, "copy w0: 0x%04X", blt_read(0x300));
    BASSERT(blt_read(0x302) == 0x5555, "copy w1: 0x%04X", blt_read(0x302));
    BASSERT(blt_read(0x304) == 0x1234, "copy w2: 0x%04X", blt_read(0x304));
    BASSERT(blt_read(0x306) == 0xDEAD, "copy w3: 0x%04X", blt_read(0x306));
}

static void test_blitter_all_256_minterms(void)
{
    /*
     * Verify all 256 minterms produce the correct output for a single
     * word with known A/B/C values.  We use A=0xAAAA, B=0xCCCC, C=0xF0F0.
     * These cover all 8 {a,b,c} input combinations across the 16 bit positions.
     *
     * For each bit position:
     *   bits 15,13,11,9,7,5,3,1: a=1,b=1,c=1 → index 7
     *   bits 14,10,6,2          : a=1,b=0,c=1 → index 5
     *   bits 12,8,4,0           : a=1,b=0,c=0 → index 4  -- wait, need rechecking
     * A=0xAAAA=1010101010101010:  bit15=1,14=0,13=1,12=0,...
     * B=0xCCCC=1100110011001100:  bit15=1,14=1,13=0,12=0,...
     * C=0xF0F0=1111000011110000:  bit15=1,14=1,13=1,12=1,11=0,10=0,9=0,8=0,...
     *
     * For each minterm mt, expected = apply_minterm(mt, 0xAAAA, 0xCCCC, 0xF0F0).
     * We verify that the blitter produces this value.
     */
    reset_agnus();
    memset(blt_ram, 0, sizeof(blt_ram));

    /* Pre-fill source words */
    blt_ram[0x100] = 0xAA; blt_ram[0x101] = 0xAA;   /* A */
    blt_ram[0x200] = 0xCC; blt_ram[0x201] = 0xCC;   /* B */
    blt_ram[0x300] = 0xF0; blt_ram[0x301] = 0xF0;   /* C */

    int errors = 0;
    for (int mt = 0; mt < 256; mt++) {
        /* Compute expected result by brute force over all 16 bits */
        uint16_t a = 0xAAAA, b = 0xCCCC, c = 0xF0F0;
        uint16_t expected = 0;
        for (int bit = 0; bit < 16; bit++) {
            int idx = ((((a >> bit) & 1u)) << 2) |
                      ((((b >> bit) & 1u)) << 1) |
                       (((c >> bit) & 1u));
            if ((mt >> idx) & 1)
                expected |= (uint16_t)(1u << bit);
        }

        /* Configure and run blitter */
        ag.bltcon0 = (uint16_t)(0x0F00u | (uint8_t)mt); /* USEA|B|C|D */
        ag.bltcon1 = 0;
        ag.bltapt  = 0x100; ag.bltbpt = 0x200; ag.bltcpt = 0x300;
        ag.bltdpt  = 0x400;
        ag.bltamod = ag.bltbmod = ag.bltcmod = ag.bltdmod = 0;
        ag.bltafwm = ag.bltalwm = 0xFFFF;

        agnus_blitter_execute(&ag, blt_ram, BLT_RAM_SIZE, (uint16_t)((1u << 6) | 1u));

        uint16_t got = blt_read(0x400);
        if (got != expected) {
            if (errors < 4)
                printf("    FAIL minterm 0x%02X: expected 0x%04X got 0x%04X\n",
                       mt, expected, got);
            errors++;
        }
        total++;   /* count each minterm as one assertion */
    }
    if (errors) failures += errors;
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

    printf("  [agnus] Copper\n");
    test_copper_move();
    test_copper_wait_future_line();
    test_copper_wait_past_line();
    test_copper_dma_disabled();

    printf("  [agnus] Blitter\n");
    test_blitter_zerofill();
    test_blitter_copy();
    test_blitter_all_256_minterms();

    printf("  [agnus] %d/%d passed\n", total - failures, total);
    return failures > 0 ? 1 : 0;
}

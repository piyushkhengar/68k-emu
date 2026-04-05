/*
 * 68010-specific internal tests.
 *
 * These tests validate features unique to the 68010:
 *   - MOVEC (read/write VBR, SFC, DFC)
 *   - RTD (return and deallocate)
 *   - Extended exception frame (8 bytes, with format/vector word)
 *   - VBR relocation (exceptions dispatched through relocated vector table)
 *
 * Each test ROM follows the same convention as the 68000 tests:
 *   - Reset vector at 0x00 (SP) and 0x04 (PC)
 *   - SP = 0x1000, code starts at 0x10
 *   - Halts with BRA.S . once the interesting work is done
 */

#include "tests_68010.h"
#include "tests_68000.h"
#include "cpu.h"
#include "cpu_internal.h"
#include "memory.h"
#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Test ROMs                                                           */
/* ------------------------------------------------------------------ */

/* Test 1: MOVEC VBR round-trip
 * Write 0x1234 into VBR via D0, then read VBR back into D1.
 * Expected: D1 == 0x1234 */
static const uint8_t rom_movec_vbr[] = {
    0x00, 0x00, 0x10, 0x00,   /* SP = 0x1000 */
    0x00, 0x00, 0x00, 0x10,   /* PC = 0x10   */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  /* padding */
    /* 0x10: MOVE.L #0x1234, D0 */
    0x20, 0x3C, 0x00, 0x00, 0x12, 0x34,
    /* 0x16: MOVEC D0, VBR  (0x4E7B ext=0x0801) */
    0x4E, 0x7B, 0x08, 0x01,
    /* 0x1A: MOVEC VBR, D1  (0x4E7A ext=0x1801) */
    0x4E, 0x7A, 0x18, 0x01,
    /* 0x1E: BRA.S . */
    0x60, 0xFE,
};

/* Test 2: MOVEC SFC/DFC round-trip
 * Write 5 → SFC, 3 → DFC, then read SFC → D1, DFC → D2.
 * Expected: D1 == 5 && D2 == 3 */
static const uint8_t rom_movec_sfc[] = {
    0x00, 0x00, 0x10, 0x00,
    0x00, 0x00, 0x00, 0x10,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    /* 0x10: MOVEQ #5, D0 */
    0x70, 0x05,
    /* 0x12: MOVEC D0, SFC  (0x4E7B ext=0x0000) */
    0x4E, 0x7B, 0x00, 0x00,
    /* 0x16: MOVEQ #3, D0 */
    0x70, 0x03,
    /* 0x18: MOVEC D0, DFC  (0x4E7B ext=0x0001) */
    0x4E, 0x7B, 0x00, 0x01,
    /* 0x1C: MOVEC SFC, D1  (0x4E7A ext=0x1000) */
    0x4E, 0x7A, 0x10, 0x00,
    /* 0x20: MOVEC DFC, D2  (0x4E7A ext=0x2001) */
    0x4E, 0x7A, 0x20, 0x01,
    /* 0x24: BRA.S . */
    0x60, 0xFE,
};

/* Test 3: RTD (return and deallocate)
 * Main pushes a dummy arg (4 bytes), calls subroutine via BSR.S.
 * Subroutine sets D0=99 then uses RTD #4 to return and discard 4 bytes.
 * Expected: D0 == 99 && A7 == 0x1000 (stack fully restored) */
static const uint8_t rom_rtd[] = {
    0x00, 0x00, 0x10, 0x00,
    0x00, 0x00, 0x00, 0x10,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    /* 0x10: MOVE.L #0xDEADBEEF, -(A7)  (push dummy arg, SP -> 0x0FFC) */
    0x2F, 0x3C, 0xDE, 0xAD, 0xBE, 0xEF,
    /* 0x16: BSR.S +8  (PC after fetch = 0x18, target = 0x20, SP -> 0x0FF8) */
    0x61, 0x08,
    /* 0x18: BRA.S .  (halt after return) */
    0x60, 0xFE,
    /* 0x1A-0x1F: padding */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    /* 0x20: MOVEQ #99, D0 */
    0x70, 0x63,
    /* 0x22: RTD #4  (pop ret addr + deallocate 4, SP: 0x0FF8 + 4 + 4 = 0x1000) */
    0x4E, 0x74, 0x00, 0x04,
};

/* Test 4: Exception frame format word
 * In 68010 mode, the top of the exception frame is a format/vector word.
 * TRAP #0 (vector 32) should push format=0, vector_offset=32*4=0x80.
 * Handler reads MOVE.W (A7), D0 to capture it.
 * Expected: D0 == 0x0080
 *
 * Memory layout:
 *   0x10: TRAP #0
 *   0x80: TRAP#0 vector → 0x00A0 (handler)
 *   0xA0: MOVE.W (A7), D0 ; BRA.S . */
static const uint8_t rom_exc_frame_fmt[0xA4] = {
    /* Reset vectors */
    [0x00] = 0x00, [0x01] = 0x00, [0x02] = 0x10, [0x03] = 0x00,
    [0x04] = 0x00, [0x05] = 0x00, [0x06] = 0x00, [0x07] = 0x10,
    /* Code at 0x10 */
    [0x10] = 0x4E, [0x11] = 0x40,   /* TRAP #0 */
    [0x12] = 0x60, [0x13] = 0xFE,   /* BRA.S . (not reached) */
    /* TRAP #0 exception vector at 0x80 (vector 32 * 4 = 0x80) */
    [0x80] = 0x00, [0x81] = 0x00, [0x82] = 0x00, [0x83] = 0xA0,
    /* Handler at 0xA0 */
    [0xA0] = 0x30, [0xA1] = 0x17,   /* MOVE.W (A7), D0  — reads format/vector word */
    [0xA2] = 0x60, [0xA3] = 0xFE,   /* BRA.S . */
};

/* Test 5: Exception frame is 8 bytes on 68010
 * Same trap setup; handler checks A7 == SP - 8.
 * Initial SSP = 0x1000; after 8-byte frame push, SSP = 0x0FF8.
 * Expected: D0 == 0x0FF8
 *
 *   0xA0: MOVE.L A7, D0 ; BRA.S . */
static const uint8_t rom_exc_frame_size[0xA4] = {
    [0x00] = 0x00, [0x01] = 0x00, [0x02] = 0x10, [0x03] = 0x00,
    [0x04] = 0x00, [0x05] = 0x00, [0x06] = 0x00, [0x07] = 0x10,
    [0x10] = 0x4E, [0x11] = 0x40,
    [0x12] = 0x60, [0x13] = 0xFE,
    [0x80] = 0x00, [0x81] = 0x00, [0x82] = 0x00, [0x83] = 0xA0,
    [0xA0] = 0x20, [0xA1] = 0x0F,   /* MOVE.L A7, D0  — captures new SSP */
    [0xA2] = 0x60, [0xA3] = 0xFE,
};

/* Test 6: VBR relocation
 * Set VBR = 0x200, then execute TRAP #0.
 * The emulator should read the TRAP #0 vector from VBR + 0x80 = 0x280.
 * Handler at 0x300 sets D0 = 99.
 * Expected: D0 == 99
 *
 *   0x10: MOVE.L #0x200, D0 ; MOVEC D0, VBR ; TRAP #0
 *   0x280: TRAP#0 vector → 0x00000300
 *   0x300: MOVEQ #99, D0 ; BRA.S . */
static const uint8_t rom_vbr_reloc[0x304] = {
    [0x00] = 0x00, [0x01] = 0x00, [0x02] = 0x10, [0x03] = 0x00,
    [0x04] = 0x00, [0x05] = 0x00, [0x06] = 0x00, [0x07] = 0x10,
    /* MOVE.L #0x200, D0 */
    [0x10] = 0x20, [0x11] = 0x3C, [0x12] = 0x00, [0x13] = 0x00,
    [0x14] = 0x02, [0x15] = 0x00,
    /* MOVEC D0, VBR */
    [0x16] = 0x4E, [0x17] = 0x7B, [0x18] = 0x08, [0x19] = 0x01,
    /* TRAP #0 */
    [0x1A] = 0x4E, [0x1B] = 0x40,
    /* BRA.S . (not reached in normal flow) */
    [0x1C] = 0x60, [0x1D] = 0xFE,
    /* TRAP #0 vector at VBR + 0x80 = 0x280 */
    [0x280] = 0x00, [0x281] = 0x00, [0x282] = 0x03, [0x283] = 0x00,
    /* Handler at 0x300 */
    [0x300] = 0x70, [0x301] = 0x63,   /* MOVEQ #99, D0 */
    [0x302] = 0x60, [0x303] = 0xFE,   /* BRA.S . */
};

/* ------------------------------------------------------------------ */
/* Test table                                                          */
/* ------------------------------------------------------------------ */

static const builtin_test_t tests[] = {
    { "movec_vbr",     rom_movec_vbr,      sizeof(rom_movec_vbr),      "MOVEC VBR round-trip",              10 },
    { "movec_sfc",     rom_movec_sfc,      sizeof(rom_movec_sfc),      "MOVEC SFC/DFC round-trip",          15 },
    { "rtd",           rom_rtd,            sizeof(rom_rtd),            "RTD stack cleanup",                 10 },
    { "exc_frame_fmt", rom_exc_frame_fmt,  sizeof(rom_exc_frame_fmt),  "68010 exception format word",       10 },
    { "exc_frame_size",rom_exc_frame_size, sizeof(rom_exc_frame_size), "68010 exception frame is 8 bytes",  10 },
    { "vbr_reloc",     rom_vbr_reloc,      sizeof(rom_vbr_reloc),      "VBR relocation dispatches correctly",15 },
};

#define NUM_TESTS (sizeof(tests) / sizeof(tests[0]))

/* ------------------------------------------------------------------ */
/* Pass/fail criteria                                                  */
/* ------------------------------------------------------------------ */

int check_68010_result(size_t idx)
{
    switch (idx) {
    case 0: return cpu.d[1] == 0x1234;
    case 1: return cpu.d[1] == 5 && cpu.d[2] == 3;
    case 2: return cpu.d[0] == 99 && cpu.a[7] == 0x1000;
    case 3: return (cpu.d[0] & 0xFFFF) == 0x0080;
    case 4: return cpu.d[0] == 0x0FF8;
    case 5: return cpu.d[0] == 99;
    default: return 0;
    }
}

/* ------------------------------------------------------------------ */
/* Runner                                                              */
/* ------------------------------------------------------------------ */

int run_68010_tests(void)
{
    int failed = 0;
    size_t n;

    printf("Running 68010 tests...\n");
    fflush(stdout);

    if (run_suite_in_mode(get_68000_tests(&n), n, check_68000_result,
                          CPU_MODEL_68010, "68000 tests in 68010 mode"))
        failed = 1;

    /* Switch to 68010 mode for native tests. */
    cpu_init(CPU_MODEL_68010);
    printf("  [native] 68010-specific tests...\n");

    for (size_t i = 0; i < NUM_TESTS; i++) {
        const builtin_test_t *t = &tests[i];

        mem_set_bus(NULL);
        mem_reset();
        mem_load_rom(t->rom, t->size);
        cpu_reset();

        int steps = 0;
        while (steps < t->max_steps) {
            int c = cpu_step();
            if (c == 0)
                break;
            cpu.cycles += c;
            steps++;
        }

        int pass = check_68010_result(i);
        if (pass) {
            printf("    %-22s PASS\n", t->name);
        } else {
            printf("    %-22s FAIL\n", t->name);
            failed = 1;
        }
    }

    /* Restore to 68000 so subsequent tests aren't affected. */
    cpu_init(CPU_MODEL_68000);

    return failed;
}

const builtin_test_t *find_68010_test(const char *name)
{
    for (size_t i = 0; i < NUM_TESTS; i++) {
        if (strcmp(tests[i].name, name) == 0)
            return &tests[i];
    }
    return NULL;
}

const builtin_test_t *get_68010_tests(size_t *out_count)
{
    *out_count = NUM_TESTS;
    return tests;
}

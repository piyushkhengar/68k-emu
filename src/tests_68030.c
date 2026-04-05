/*
 * 68030-specific internal tests.
 *
 * The 68030 integer ISA is identical to the 68020, so these tests focus on
 * the features that first appear on the 68030:
 *
 *   - MOVEC CACR and CAAR: cache control registers (first fully wired on 68030)
 *   - PMOVE: transfer between memory and MMU registers (TC, SRP, CRP, TT0, TT1)
 *   - PFLUSH / PFLUSHA: TLB invalidation (stubbed as no-op in this emulator)
 *
 * All PMOVE and PFLUSH instructions are privileged.  After reset the CPU is in
 * supervisor mode (SR bit 13 = S = 1), so tests run without privilege faults.
 *
 * Each ROM follows the same setup convention as the other model tests:
 *   - Reset vector at 0x00 (SP = 0x1000) and 0x04 (PC = 0x10)
 *   - Code starts at 0x10
 *   - Halts with BRA.S . once the interesting work is done
 *
 * PMOVE extension word encoding (confirmed from MC68030 User's Manual examples):
 *   bits 15-13 = 010 (0x4000): read MMU register → memory  (e.g. PMOVE TC, (An))
 *   bits 15-13 = 011 (0x6000): write memory → MMU register (e.g. PMOVE (An), TC)
 *   bits 15-13 = 001 (0x2400): PFLUSH / PFLUSHA
 */

#include "tests_68030.h"
#include "tests_68000.h"
#include "tests_68010.h"
#include "tests_68020.h"
#include "cpu.h"
#include "cpu_internal.h"
#include "memory.h"
#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Test ROMs                                                           */
/* ------------------------------------------------------------------ */

/*
 * Test 1: MOVEC CACR round-trip (Cache Control Register, cr=0x002)
 *
 *   MOVEQ  #1, D0          ; value to write
 *   MOVEC  D0, CACR        ; D0 → CACR  (opcode 0x4E7B, ext 0x0002)
 *   MOVEC  CACR, D1        ; CACR → D1  (opcode 0x4E7A, ext 0x1002)
 *   BRA.S  .
 *
 * Expected: D1 = 1  (round-trips the value through CACR)
 */
static const uint8_t rom_cacr_rw[] = {
    0x00, 0x00, 0x10, 0x00,   /* SP = 0x1000 */
    0x00, 0x00, 0x00, 0x10,   /* PC = 0x10   */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  /* padding */
    /* 0x10: MOVEQ #1, D0 */
    0x70, 0x01,
    /* 0x12: MOVEC D0, CACR  (0x4E7B, ext = D/A=0, reg=0, cr=0x002) */
    0x4E, 0x7B, 0x00, 0x02,
    /* 0x16: MOVEC CACR, D1  (0x4E7A, ext = D/A=0, reg=1, cr=0x002) */
    0x4E, 0x7A, 0x10, 0x02,
    /* 0x1A: BRA.S . */
    0x60, 0xFE,
};

/*
 * Test 2: MOVEC CAAR round-trip (Cache Address Register, cr=0x802)
 *
 *   MOVEQ  #0x2A, D0       ; value to write (42 decimal)
 *   MOVEC  D0, CAAR        ; D0 → CAAR  (opcode 0x4E7B, ext 0x0802)
 *   MOVEC  CAAR, D1        ; CAAR → D1  (opcode 0x4E7A, ext 0x1802)
 *   BRA.S  .
 *
 * Extension word layout: D/A=0 (data reg), reg number in bits 14-12, cr in bits 11-0.
 *   MOVEC D0, CAAR: bit15=0, bits14-12=000 (D0), bits11-0=0x802 → 0x0802
 *   MOVEC CAAR, D1: bit15=0, bits14-12=001 (D1), bits11-0=0x802 → 0x1802
 *
 * Expected: D1 = 0x2A
 */
static const uint8_t rom_caar_rw[] = {
    0x00, 0x00, 0x10, 0x00,
    0x00, 0x00, 0x00, 0x10,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    /* 0x10: MOVEQ #0x2A, D0 */
    0x70, 0x2A,
    /* 0x12: MOVEC D0, CAAR  (0x4E7B, ext 0x0802) */
    0x4E, 0x7B, 0x08, 0x02,
    /* 0x16: MOVEC CAAR, D1  (0x4E7A, ext 0x1802) */
    0x4E, 0x7A, 0x18, 0x02,
    /* 0x1A: BRA.S . */
    0x60, 0xFE,
};

/*
 * Test 3: PMOVE round-trip for TC (Translation Control register, 32-bit)
 *
 * Writes a known value to memory at address 0x100, uses PMOVE to load it
 * into the TC register, then reads TC back to address 0x104, and finally
 * loads that word into D0.
 *
 *   MOVE.L #0x12345678, ($0100).W    ; store test value to memory
 *   MOVEA.L #0x100, A0               ; A0 → source
 *   PMOVE  (A0), TC                  ; mem[0x100] → TC  (ext = 0x6000)
 *   MOVEA.L #0x104, A1               ; A1 → dest
 *   PMOVE  TC, (A1)                  ; TC → mem[0x104] (ext = 0x4000)
 *   MOVE.L ($0104).W, D0             ; D0 = mem[0x104]
 *   BRA.S  .
 *
 * PMOVE opcode word: 1111 000 000 ea_mode ea_reg = 0xF000 | ea_encoding
 *   (A0) = mode 2, reg 0 → bits 5-0 = 010 000 = 0x10 → opcode 0xF010
 *   (A1) = mode 2, reg 1 → bits 5-0 = 010 001 = 0x11 → opcode 0xF011
 *
 * PMOVE extension words (from MC68030 UM examples):
 *   0x6000 = 0110 0000... → bits 15-13 = 011 = write EA→TC
 *   0x4000 = 0100 0000... → bits 15-13 = 010 = read TC→EA
 *
 * MOVE.L encoding used here:
 *   MOVE.L #imm, ($nn).W: 0x21FC, then 4-byte imm, then 2-byte abs address
 *   MOVEA.L #imm, An:    0x207C/0x227C/..., then 4-byte imm
 *   MOVE.L ($nn).W, Dn:  0x2038 for D0, then 2-byte abs address
 *
 * Expected: D0 = 0x12345678
 */
static const uint8_t rom_tc_pmove[] = {
    0x00, 0x00, 0x10, 0x00,   /* SP = 0x1000 */
    0x00, 0x00, 0x00, 0x10,   /* PC = 0x10   */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  /* padding */

    /* 0x10: MOVE.L #0x12345678, ($0100).W
     * opcode 0x21FC = 0010 0001 1111 1100
     *   bits15-12=0010 (MOVE.L), bits11-9=000 (dst reg for abs.w),
     *   bits8-6=111 (dst mode=7), bits5-3=111 (src mode=7), bits2-0=100 (#imm) */
    0x21, 0xFC, 0x12, 0x34, 0x56, 0x78, 0x01, 0x00,

    /* 0x18: MOVEA.L #0x100, A0  (opcode 0x207C = MOVEA.L imm, A0) */
    0x20, 0x7C, 0x00, 0x00, 0x01, 0x00,

    /* 0x1E: PMOVE (A0), TC  (opcode 0xF010, ext 0x6000) */
    0xF0, 0x10, 0x60, 0x00,

    /* 0x22: MOVEA.L #0x104, A1  (opcode 0x227C = MOVEA.L imm, A1) */
    0x22, 0x7C, 0x00, 0x00, 0x01, 0x04,

    /* 0x28: PMOVE TC, (A1)  (opcode 0xF011, ext 0x4000) */
    0xF0, 0x11, 0x40, 0x00,

    /* 0x2C: MOVE.L ($0104).W, D0
     * opcode 0x2038 = 0010 0000 0011 1000
     *   bits15-12=0010 (MOVE.L), bits11-9=000 (dst D0), bits8-6=000 (dst mode=0/Dn),
     *   bits5-3=111 (src mode=7), bits2-0=000 (src reg=0/abs.w) */
    0x20, 0x38, 0x01, 0x04,

    /* 0x30: BRA.S . */
    0x60, 0xFE,
};

/*
 * Test 4: PFLUSHA — flush all TLB entries (no-op stub)
 *
 *   MOVEQ  #99, D0         ; sentinel value
 *   PFLUSHA                ; F000 2400 — flush all; no-op in this emulator
 *   BRA.S  .
 *
 * PFLUSHA opcode: 0xF000 (EA=D0, mode=0 reg=0 — effectively no EA operand)
 * Extension word: 0x2400 = 0010 0100 0000 0000 → bits 15-13 = 001 = PFLUSH
 *
 * If PFLUSHA were incorrectly dispatched to op_unimplemented, the CPU would
 * take a LINE1111 exception and halt.  Reaching BRA.S . proves it executes.
 *
 * Expected: D0 = 99 (sentinel unchanged)
 */
static const uint8_t rom_pflush_noop[] = {
    0x00, 0x00, 0x10, 0x00,
    0x00, 0x00, 0x00, 0x10,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    /* 0x10: MOVEQ #99, D0 */
    0x70, 0x63,
    /* 0x12: PFLUSHA  (opcode 0xF000, ext 0x2400) */
    0xF0, 0x00, 0x24, 0x00,
    /* 0x16: BRA.S . */
    0x60, 0xFE,
};

/* ------------------------------------------------------------------ */
/* Test table                                                          */
/* ------------------------------------------------------------------ */

static const builtin_test_t tests[] = {
    { "cacr_rw",      rom_cacr_rw,      sizeof(rom_cacr_rw),      "MOVEC CACR round-trip",             6 },
    { "caar_rw",      rom_caar_rw,      sizeof(rom_caar_rw),      "MOVEC CAAR round-trip",             6 },
    { "tc_pmove",     rom_tc_pmove,     sizeof(rom_tc_pmove),     "PMOVE TC: write then read back",    9, CPU_MODEL_68030 },
    { "pflush_noop",  rom_pflush_noop,  sizeof(rom_pflush_noop),  "PFLUSHA: no-op, sentinel preserved", 4, CPU_MODEL_68030 },
};

#define NUM_TESTS (sizeof(tests) / sizeof(tests[0]))

/* ------------------------------------------------------------------ */
/* Pass/fail criteria                                                  */
/* ------------------------------------------------------------------ */

int check_68030_result(size_t idx)
{
    switch (idx) {
    case 0: return cpu.d[1] == 1;           /* MOVEC CACR round-trip: D1 = 1 */
    case 1: return cpu.d[1] == 0x2A;        /* MOVEC CAAR round-trip: D1 = 0x2A */
    case 2: return cpu.d[0] == 0x12345678;  /* PMOVE TC: round-trip value preserved */
    case 3: return cpu.d[0] == 99;          /* PFLUSHA: sentinel D0 = 99 */
    default: return 0;
    }
}

/* ------------------------------------------------------------------ */
/* Runner                                                              */
/* ------------------------------------------------------------------ */

int run_68030_tests(void)
{
    int failed = 0;
    size_t n;

    printf("Running 68030 tests...\n");
    fflush(stdout);

    if (run_suite_in_mode(get_68000_tests(&n), n, check_68000_result,
                          CPU_MODEL_68030, "68000 tests in 68030 mode"))
        failed = 1;
    if (run_suite_in_mode(get_68010_tests(&n), n, check_68010_result,
                          CPU_MODEL_68030, "68010 tests in 68030 mode"))
        failed = 1;
    if (run_suite_in_mode(get_68020_tests(&n), n, check_68020_result,
                          CPU_MODEL_68030, "68020 tests in 68030 mode"))
        failed = 1;

    /* Switch to 68030 mode for native tests. */
    cpu_init(CPU_MODEL_68030);
    printf("  [native] 68030-specific tests...\n");

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

        int pass = check_68030_result(i);
        if (pass) {
            printf("    %-22s PASS\n", t->name);
        } else {
            printf("    %-22s FAIL\n", t->name);
            failed = 1;
        }
    }

    /* Restore to 68000 so subsequent tests are not affected. */
    cpu_init(CPU_MODEL_68000);

    return failed;
}

const builtin_test_t *find_68030_test(const char *name)
{
    for (size_t i = 0; i < NUM_TESTS; i++) {
        if (strcmp(tests[i].name, name) == 0)
            return &tests[i];
    }
    return NULL;
}

const builtin_test_t *get_68030_tests(size_t *out_count)
{
    *out_count = NUM_TESTS;
    return tests;
}

/*
 * 68060-specific internal tests.
 *
 * The 68060 is functionally identical to the 68040 except for two changes
 * relevant to this emulator:
 *
 *   1. New instruction: LPSTOP #imm (0xF800 + 16-bit immediate)
 *      Low-power privileged halt; loads SR from the immediate and sets
 *      cpu.halted = 1 until an interrupt arrives.
 *
 *   2. Removed instruction: MOVEP
 *      The 68060 dropped MOVEP in hardware.  Executing a MOVEP opcode
 *      raises a Line-1111 exception (vector 11).
 *
 * Setup convention (same as all other model test files):
 *   - Reset vector at 0x00 (SP = 0x1000) and 0x04 (PC = 0x10)
 *   - Code starts at 0x10
 *   - Halts with BRA.S . or via cpu.halted == 1
 *
 * LPSTOP encoding:
 *   Opword:     0xF8 0x00  (= 0xF800)
 *   Extension:  16-bit immediate (written to SR on entry)
 *
 * MOVEP.W Dn, d(An) encoding (e.g. MOVEP.W D0, 0(A0)):
 *   Opword:     0x01 0x88  (= 0x0188)
 *   Disp:       0x00 0x00  (zero displacement)
 *   Pattern: opcode & 0xF1F8 == 0x0188 for MOVEP.W Dn,d(An)
 *
 * Line-1111 vector is at byte offset 11*4 = 0x2C in the vector table.
 *
 * Exception handler sentinel:
 *   MOVE.L #0xDEAD, D1  (0x22 3C 00 00 DE AD) sets D1 when the trap fires.
 *   BRA.S .             (0x60 FE) keeps it halted there.
 *
 * SR interrupt-priority mask:
 *   SR_I_MASK = 0x0700 (bits 10-8)
 *   LPSTOP #0x2500 → SR = 0x2500 → (SR & 0x0700) == 0x0500 (IPL 5)
 */

#include "tests_68060.h"
#include "cpu.h"
#include "cpu_internal.h"
#include "memory.h"
#include <stdio.h>

/* ------------------------------------------------------------------ */
/* Test ROMs                                                           */
/* ------------------------------------------------------------------ */

/*
 * Test 1: LPSTOP basic sentinel
 *
 *   MOVEQ  #77, D0        ; sentinel value (must survive)
 *   LPSTOP #0x2700        ; load SR=0x2700, set halted=1
 *
 * Expected: cpu.d[0] == 77 && cpu.halted == 1
 */
static const uint8_t rom_lpstop_sentinel[] = {
    0x00, 0x00, 0x10, 0x00,   /* SP = 0x1000 */
    0x00, 0x00, 0x00, 0x10,   /* PC = 0x10   */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  /* padding */
    /* 0x10: MOVEQ #77, D0 */
    0x70, 0x4D,
    /* 0x12: LPSTOP #0x2700  (opword 0xF800, ext 0x2700) */
    0xF8, 0x00, 0x27, 0x00,
};

/*
 * Test 2: LPSTOP loads SR correctly
 *
 *   LPSTOP #0x2500        ; SR = 0x2500 → interrupt mask = IPL 5
 *
 * Expected: (cpu.sr & SR_I_MASK) == 0x0500
 */
static const uint8_t rom_lpstop_sr[] = {
    0x00, 0x00, 0x10, 0x00,
    0x00, 0x00, 0x00, 0x10,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    /* 0x10: LPSTOP #0x2500  (opword 0xF800, ext 0x2500) */
    0xF8, 0x00, 0x25, 0x00,
};

/*
 * Test 3: MOVEP raises Line-1111 exception on 68060
 *
 * ROM layout:
 *   0x00-0x03  SP = 0x1000
 *   0x04-0x07  PC = 0x40      (code starts at 0x40, after vector table)
 *   0x2C-0x2F  vector 11 = 0x00000050  (Line-1111 handler address)
 *   0x40-0x47  MOVEP.W D0, 0(A0)  then BRA.S .
 *   0x50-0x57  handler: MOVE.L #0xDEAD, D1  then BRA.S .
 *
 * MOVEP.W D0, 0(A0):
 *   opword 0x0188, displacement 0x0000
 *   opcode & 0xF1F8 == 0x0188 ← matches has_movep=0 gate in dispatch_0xxx
 *
 * Handler at 0x50:
 *   MOVE.L #0xDEAD, D1  (0x22 3C 00 00 DE AD)
 *   BRA.S .             (0x60 FE)
 *
 * Expected: cpu.d[1] == 0xDEAD
 */
static const uint8_t rom_movep_line1111[] = {
    /* 0x00: SP = 0x1000 */
    0x00, 0x00, 0x10, 0x00,
    /* 0x04: PC = 0x40 */
    0x00, 0x00, 0x00, 0x40,
    /* 0x08-0x2B: vectors 2-10 (unused, zero) */
    0x00, 0x00, 0x00, 0x00,  /* vector 2 */
    0x00, 0x00, 0x00, 0x00,  /* vector 3 */
    0x00, 0x00, 0x00, 0x00,  /* vector 4 */
    0x00, 0x00, 0x00, 0x00,  /* vector 5 */
    0x00, 0x00, 0x00, 0x00,  /* vector 6 */
    0x00, 0x00, 0x00, 0x00,  /* vector 7 */
    0x00, 0x00, 0x00, 0x00,  /* vector 8 */
    0x00, 0x00, 0x00, 0x00,  /* vector 9 */
    0x00, 0x00, 0x00, 0x00,  /* vector 10 */
    /* 0x2C: vector 11 = Line-1111 handler @ 0x50 */
    0x00, 0x00, 0x00, 0x50,
    /* 0x30-0x3F: vectors 12-15 (unused) */
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
    /* 0x40: MOVEP.W D0, 0(A0)  (opword 0x0188, disp 0x0000) */
    0x01, 0x88, 0x00, 0x00,
    /* 0x44: BRA.S .  (should not be reached if exception fires) */
    0x60, 0xFE,
    /* 0x46-0x4F: padding */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    /* 0x50: handler: MOVE.L #0xDEAD, D1  (0x223C, then 4-byte imm) */
    0x22, 0x3C, 0x00, 0x00, 0xDE, 0xAD,
    /* 0x56: BRA.S .  (halt in handler) */
    0x60, 0xFE,
};

/* ------------------------------------------------------------------ */
/* Test table                                                          */
/* ------------------------------------------------------------------ */

typedef struct {
    const char    *name;
    const uint8_t *rom;
    size_t         size;
    const char    *description;
    int            max_steps;
} test68060_t;

static const test68060_t tests[] = {
    { "lpstop_sentinel",  rom_lpstop_sentinel,  sizeof(rom_lpstop_sentinel),  "LPSTOP: D0 sentinel preserved, halted=1",    3 },
    { "lpstop_sr",        rom_lpstop_sr,        sizeof(rom_lpstop_sr),        "LPSTOP #0x2500: SR interrupt mask = IPL 5",  2 },
    { "movep_line1111",   rom_movep_line1111,   sizeof(rom_movep_line1111),   "MOVEP on 68060 raises Line-1111 exception",  6 },
};

#define NUM_TESTS (sizeof(tests) / sizeof(tests[0]))

/* ------------------------------------------------------------------ */
/* Pass/fail criteria                                                  */
/* ------------------------------------------------------------------ */

static int check_result(size_t idx)
{
    switch (idx) {
    case 0: return cpu.d[0] == 77 && cpu.halted == 1;
    case 1: return (cpu.sr & SR_I_MASK) == 0x0500;
    case 2: return cpu.d[1] == 0xDEAD;
    default: return 0;
    }
}

/* ------------------------------------------------------------------ */
/* Runner                                                              */
/* ------------------------------------------------------------------ */

int run_68060_tests(void)
{
    int failed = 0;

    printf("Running 68060 tests...\n");
    fflush(stdout);

    /* Switch to 68060 mode for this suite. */
    cpu_init(CPU_MODEL_68060);

    for (size_t i = 0; i < NUM_TESTS; i++) {
        const test68060_t *t = &tests[i];

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

        int pass = check_result(i);
        if (pass) {
            printf("  %-22s PASS\n", t->name);
        } else {
            printf("  %-22s FAIL\n", t->name);
            failed = 1;
        }
    }

    /* Restore to 68000 so subsequent tests are not affected. */
    cpu_init(CPU_MODEL_68000);

    return failed;
}

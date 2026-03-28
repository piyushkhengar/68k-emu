/*
 * 68040-specific internal tests.
 *
 * The 68040 integer ISA is identical to the 68020/68030, so these tests focus
 * on the features that first appear on the 68040 (or change architecture on it):
 *
 *   - MOVEC to/from 68040 MMU registers (TC, URP, ITT0) — formerly PMOVE on 68030
 *   - CINVA: cache invalidate all (privileged no-op in this emulator)
 *   - MOVE16 (Ax)+, (Ay)+: 16-byte cache-efficient block copy
 *   - FMOVE.L <ea>, FPn / FMOVE.L FPn, <ea>: integer load/store round-trip
 *   - FSAVE / FRESTORE: FPU context save/restore (null frame stubs)
 *
 * All instructions are privileged (supervisor only).  After reset the CPU is in
 * supervisor mode (SR bit 13 = S = 1), so tests run without privilege faults.
 *
 * Each ROM follows the same setup convention as other model tests:
 *   - Reset vector at 0x00 (SP = 0x1000) and 0x04 (PC = 0x10)
 *   - Code starts at 0x10
 *   - Halts with BRA.S . once the interesting work is done
 *
 * MOVEC extension word (bits 15-0):
 *   bit 15   : 0=Dn, 1=An (source/destination register type)
 *   bits 14-12: register number (0-7)
 *   bits 11-0 : control register number
 *
 *   MOVEC D0, CR  (0x4E7B): ext = 0x0000 | (cr & 0xFFF)
 *   MOVEC CR, D1  (0x4E7A): ext = 0x1000 | (cr & 0xFFF)
 *
 * FMOVE extension word (bit 14=1 = EA involved, bit 13=0 = load EA→FPn):
 *   FMOVE.L (An), FP0: ext = 0x4000  (bit14=1, bit13=0, fmt=000=long, FP0)
 *   FMOVE.L FP0, (An): ext = 0x6000  (bit14=1, bit13=1, fmt=000=long, FP0)
 *
 * FPU cpGEN first opword: 0xF200 | ea_encoding
 *   (An) = mode 2, reg n → ea bits 5-3=010, 2-0=n → bits 5-0 = 0b010nnn
 *   FMOVE.L (A0), FP0: 0xF200 | 0x10 = 0xF210, ext = 0x4000
 *   FMOVE.L FP0, (A1): 0xF200 | 0x11 = 0xF211, ext = 0x6000
 *
 * MOVE16 (Ax)+, (Ay)+ encoding:
 *   First word:  1111 0110 0010 0 Ax = 0xF620 | Ax
 *   Second word: 1 Ay 000 0000 0000 0 = 0x8000 | (Ay << 12)
 *   For A0→A1: 0xF620, 0x9000
 *
 * CINVA IC = 0xF498 (cache invalidate all, instruction cache)
 *
 * FSAVE -(A7): 0xF327  (cpSAVE: 0xF300 | mode7=011 reg7=111 = 0x1F → 0xF31F?
 *   cpSAVE: 1111 001 110 ea_mode ea_reg  → bits 11-9 = 110
 *   FSAVE -(A7) = predecrement A7: ea_mode=100, ea_reg=111 → bits 5-0 = 100 111 = 0x27
 *   opword = 0xF300 | (6<<6) | 0x27 = 0xF300 | 0x180 | 0x27 = 0xF327?
 *   Actually: 1111 001 110 ea_mode ea_reg
 *     bits 11-9 = 110 (cpSAVE sub-type 6)
 *     ea = -(A7): mode=100(4), reg=111(7) → bits 5-3=100, 2-0=111 → 0x27
 *     opword = 1111 0 01 110 100 111 = F327? Let me re-derive:
 *     bit15-12 = 1111
 *     bit11-9  = 001 (cpGEN sub-type 0? No — FSAVE is sub-type 6)
 *     bit8-6   = 110 (sub within cpGEN? or the top 3 bits of the operand type?)
 *   The top 4 bits of op (bits 15-12) = 1111 (F).
 *   Bits 11-9 of op select the cpID/type for the 68040:
 *     001 = FPU coprocessor (so bits 11-9 = 001)
 *   Bits 8-6 select the instruction sub-type:
 *     000 = cpGEN, 110 = cpSAVE, 111 = cpRESTORE
 *
 *   FSAVE -(A7):
 *     bits 15-12 = 1111
 *     bits 11-9  = 001 (FPU)
 *     bits 8-6   = 110 (FSAVE)
 *     bits 5-0   = ea for -(A7) = mode=100, reg=111 = 0b100111 = 0x27
 *     = 0xF200 | (6<<6) | 0x27 = 0xF200 | 0x180 | 0x27 = 0xF3A7
 *   Wait: (6 << 6) = 0x180 and 0xF200 | 0x180 = 0xF380. Then | 0x27 = 0xF3A7.
 *   Hmm, but bits 11-9 = 001 means the value 0x200 is already in 0xF200.
 *   0xF200 = 1111 0010 0000 0000:
 *     bits 15-12 = 1111 ✓
 *     bits 11-9  = 001 ✓
 *     bits 8-6   = 000 (cpGEN)
 *   For FSAVE (cpSAVE, sub=6): bits 8-6 = 110 → add (6 << 6) = 0x180
 *   0xF200 | 0x180 = 0xF380
 *   With -(A7) EA (mode=4, reg=7): 0xF380 | 0x27 = 0xF3A7
 *
 *   FRESTORE (A7)+:
 *     bits 8-6 = 111 (cpRESTORE) → 0xF200 | (7<<6) = 0xF200 | 0x1C0 = 0xF3C0
 *     With (A7)+ EA (mode=3, reg=7): 0xF3C0 | 0x1F = 0xF3DF
 *
 * CR numbers:
 *   TC   = 0x003
 *   ITT0 = 0x004
 *   URP  = 0x806
 */

#include "tests_68040.h"
#include "cpu.h"
#include "cpu_internal.h"
#include "memory.h"
#include <stdio.h>

/* ------------------------------------------------------------------ */
/* Test ROMs                                                           */
/* ------------------------------------------------------------------ */

/*
 * Test 1: MOVEC TC round-trip (Translation Control register, cr=0x003)
 *
 *   MOVEQ  #0x55, D0        ; value to write
 *   MOVEC  D0, TC           ; D0 → TC  (0x4E7B, ext D/A=0 reg=0 cr=0x003 → 0x0003)
 *   MOVEC  TC, D1           ; TC → D1  (0x4E7A, ext D/A=0 reg=1 cr=0x003 → 0x1003)
 *   BRA.S  .
 *
 * Expected: D1 = 0x55
 */
static const uint8_t rom_tc_movec[] = {
    0x00, 0x00, 0x10, 0x00,   /* SP = 0x1000 */
    0x00, 0x00, 0x00, 0x10,   /* PC = 0x10   */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  /* padding */
    /* 0x10: MOVEQ #0x55, D0 */
    0x70, 0x55,
    /* 0x12: MOVEC D0, TC  (0x4E7B, ext 0x0003) */
    0x4E, 0x7B, 0x00, 0x03,
    /* 0x16: MOVEC TC, D1  (0x4E7A, ext 0x1003) */
    0x4E, 0x7A, 0x10, 0x03,
    /* 0x1A: BRA.S . */
    0x60, 0xFE,
};

/*
 * Test 2: MOVEC URP round-trip (User Root Pointer, cr=0x806)
 *
 *   MOVE.L #0x2000, D0      ; value to write (larger than MOVEQ range)
 *   MOVEC  D0, URP           ; D0 → URP  (0x4E7B, ext 0x0806)
 *   MOVEC  URP, D1           ; URP → D1  (0x4E7A, ext 0x1806)
 *   BRA.S  .
 *
 * MOVE.L #imm, D0 encoding:
 *   0x203C = 0010 0000 0011 1100
 *   bits15-12=0010 (MOVE.L), bits11-9=000 (D0 dest), bits8-6=000 (Dn mode),
 *   bits5-3=111 (mode 7), bits2-0=100 (#imm) → followed by 4-byte immediate
 *
 * Expected: D1 = 0x2000
 */
static const uint8_t rom_urp_movec[] = {
    0x00, 0x00, 0x10, 0x00,
    0x00, 0x00, 0x00, 0x10,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    /* 0x10: MOVE.L #0x2000, D0  (0x203C, then 4 bytes) */
    0x20, 0x3C, 0x00, 0x00, 0x20, 0x00,
    /* 0x16: MOVEC D0, URP  (0x4E7B, ext 0x0806) */
    0x4E, 0x7B, 0x08, 0x06,
    /* 0x1A: MOVEC URP, D1  (0x4E7A, ext 0x1806) */
    0x4E, 0x7A, 0x18, 0x06,
    /* 0x1E: BRA.S . */
    0x60, 0xFE,
};

/*
 * Test 3: MOVEC ITT0 round-trip (Instruction Transparent Translation 0, cr=0x004)
 *
 *   MOVEQ  #0x77, D0
 *   MOVEC  D0, ITT0         ; (0x4E7B, ext 0x0004)
 *   MOVEC  ITT0, D1         ; (0x4E7A, ext 0x1004)
 *   BRA.S  .
 *
 * Expected: D1 = 0x77
 */
static const uint8_t rom_itt0_movec[] = {
    0x00, 0x00, 0x10, 0x00,
    0x00, 0x00, 0x00, 0x10,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    /* 0x10: MOVEQ #0x77, D0 */
    0x70, 0x77,
    /* 0x12: MOVEC D0, ITT0  (0x4E7B, ext 0x0004) */
    0x4E, 0x7B, 0x00, 0x04,
    /* 0x16: MOVEC ITT0, D1  (0x4E7A, ext 0x1004) */
    0x4E, 0x7A, 0x10, 0x04,
    /* 0x1A: BRA.S . */
    0x60, 0xFE,
};

/*
 * Test 4: CINVA IC — cache invalidate all, instruction cache (no-op stub)
 *
 *   MOVEQ  #99, D0          ; sentinel value
 *   CINVA  IC               ; 0xF498 — invalidate all IC; no-op in this emulator
 *   BRA.S  .
 *
 * CINVA IC encoding: 0xF498 = 1111 0100 1001 1000
 *   bits 15-8 = 0xF4 (dispatch to op_cache_dispatch)
 *   bits 7-6 = 10 (scope = all)
 *   bits 5-4 = 01 (IC = instruction cache)
 *   bits 3-0 = 1000 (no register operand)
 *
 * If CINVA were unimplemented and threw a LINE1111 exception, D0 would change.
 * Expected: D0 = 99 (sentinel unchanged, no exception taken)
 */
static const uint8_t rom_cinva_noop[] = {
    0x00, 0x00, 0x10, 0x00,
    0x00, 0x00, 0x00, 0x10,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    /* 0x10: MOVEQ #99, D0 */
    0x70, 0x63,
    /* 0x12: CINVA IC  (0xF498) */
    0xF4, 0x98,
    /* 0x14: BRA.S . */
    0x60, 0xFE,
};

/*
 * Test 5: MOVE16 (A0)+, (A1)+ basic copy
 *
 * Writes 0xDEADBEEF at address 0x100 (16-byte aligned), then copies 16 bytes
 * from 0x100 to 0x200 using MOVE16.  Reads back D0 from 0x200.
 *
 *   MOVE.L  #0xDEADBEEF, ($0100).W    ; store test pattern at 0x100
 *   MOVEA.L #0x100, A0                ; source
 *   MOVEA.L #0x200, A1                ; destination
 *   MOVE16  (A0)+, (A1)+              ; copy 16 bytes (0xF620, 0x9000)
 *   MOVE.L  ($0200).W, D0             ; D0 = first 4 bytes of copy
 *   BRA.S   .
 *
 * MOVE16 (A0)+, (A1)+:
 *   First word:  0xF620 | 0 (A0) = 0xF620
 *   Second word: 0x8000 | (1 << 12) (A1) = 0x9000
 *
 * MOVE.L #imm, ($nn).W:  0x21FC then 4-byte imm, 2-byte abs address
 * MOVEA.L #imm, An:      0x207C (A0), 0x227C (A1) then 4-byte imm
 * MOVE.L ($nn).W, D0:    0x2038 then 2-byte abs address
 *
 * Expected: D0 = 0xDEADBEEF
 */
static const uint8_t rom_move16_copy[] = {
    0x00, 0x00, 0x10, 0x00,   /* SP = 0x1000 */
    0x00, 0x00, 0x00, 0x10,   /* PC = 0x10   */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  /* padding */

    /* 0x10: MOVE.L #0xDEADBEEF, ($0100).W
     * opcode 0x21FC: MOVE.L #imm, abs.W */
    0x21, 0xFC, 0xDE, 0xAD, 0xBE, 0xEF, 0x01, 0x00,

    /* 0x18: MOVEA.L #0x100, A0  (0x207C) */
    0x20, 0x7C, 0x00, 0x00, 0x01, 0x00,

    /* 0x1E: MOVEA.L #0x200, A1  (0x227C) */
    0x22, 0x7C, 0x00, 0x00, 0x02, 0x00,

    /* 0x24: MOVE16 (A0)+, (A1)+  (0xF620, 0x9000) */
    0xF6, 0x20, 0x90, 0x00,

    /* 0x28: MOVE.L ($0200).W, D0  (0x2038, abs) */
    0x20, 0x38, 0x02, 0x00,

    /* 0x2C: BRA.S . */
    0x60, 0xFE,
};

/*
 * Test 6: FMOVE.L integer round-trip via FP0
 *
 * Stores the value 42 in memory at 0x108, loads it into FP0 via FMOVE.L,
 * then stores FP0 back to 0x10C via FMOVE.L FP0, (A1).  Finally reads 0x10C
 * into D1 and checks it equals 42.
 *
 *   MOVE.L  #42, ($0108).W           ; store 42 at 0x108
 *   MOVEA.L #0x108, A0               ; A0 = source address
 *   MOVEA.L #0x10C, A1               ; A1 = destination address
 *   FMOVE.L (A0), FP0                ; 0xF210, ext 0x4000
 *   FMOVE.L FP0, (A1)                ; 0xF211, ext 0x6000
 *   MOVE.L  ($010C).W, D1            ; D1 = readback
 *   BRA.S   .
 *
 * FMOVE extension words:
 *   Load  EA→FP0: bit14=1, bit13=0, fmt=000(long), FP0 → ext = 0x4000
 *   Store FP0→EA: bit14=1, bit13=1, fmt=000(long), FP0 → ext = 0x6000
 *
 * Expected: D1 = 42
 */
static const uint8_t rom_fmove_rtrip[] = {
    0x00, 0x00, 0x10, 0x00,   /* SP = 0x1000 */
    0x00, 0x00, 0x00, 0x10,   /* PC = 0x10   */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  /* padding */

    /* 0x10: MOVE.L #42, ($0108).W  (0x21FC, imm, abs.W) */
    0x21, 0xFC, 0x00, 0x00, 0x00, 0x2A, 0x01, 0x08,

    /* 0x18: MOVEA.L #0x108, A0  (0x207C) */
    0x20, 0x7C, 0x00, 0x00, 0x01, 0x08,

    /* 0x1E: MOVEA.L #0x10C, A1  (0x227C) */
    0x22, 0x7C, 0x00, 0x00, 0x01, 0x0C,

    /* 0x24: FMOVE.L (A0), FP0  — opword 0xF210, ext 0x4000 */
    0xF2, 0x10, 0x40, 0x00,

    /* 0x28: FMOVE.L FP0, (A1)  — opword 0xF211, ext 0x6000 */
    0xF2, 0x11, 0x60, 0x00,

    /* 0x2C: MOVE.L ($010C).W, D1  (0x2238 = MOVE.L abs.W, D1) */
    0x22, 0x38, 0x01, 0x0C,

    /* 0x30: BRA.S . */
    0x60, 0xFE,
};

/*
 * Test 7: FSAVE / FRESTORE no-crash sentinel test
 *
 *   MOVEQ  #77, D0          ; sentinel
 *   FSAVE  -(A7)            ; push null FPU state frame; 0xF327
 *   FRESTORE (A7)+          ; pop it back; 0xF35F
 *   BRA.S  .
 *
 * FSAVE -(A7):
 *   bits 15-12 = 1111
 *   bits 11-9  = 001 (FPU coprocessor)
 *   bits 8-6   = 110 (cpSAVE sub-type 6)
 *   EA = -(A7): mode=100(4), reg=111(7) → bits 5-0 = 100 111 = 0x27
 *   opword = 0xF200 | (6<<6) | 0x27 = 0xF380 | 0x27 = 0xF3A7
 *
 * FRESTORE (A7)+:
 *   bits 8-6   = 111 (cpRESTORE sub-type 7)
 *   EA = (A7)+: mode=011(3), reg=111(7) → bits 5-0 = 011 111 = 0x1F
 *   opword = 0xF200 | (7<<6) | 0x1F = 0xF3C0 | 0x1F = 0xF3DF
 *
 * Expected: D0 = 77 (sentinel unchanged, no exception taken)
 */
static const uint8_t rom_fsave_frestore[] = {
    0x00, 0x00, 0x10, 0x00,
    0x00, 0x00, 0x00, 0x10,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    /* 0x10: MOVEQ #77, D0 */
    0x70, 0x4D,
    /* 0x12: FSAVE -(A7)  (0xF3A7) */
    0xF3, 0xA7,
    /* 0x14: FRESTORE (A7)+  (0xF3DF) */
    0xF3, 0xDF,
    /* 0x16: BRA.S . */
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
} test68040_t;

static const test68040_t tests[] = {
    { "tc_movec",       rom_tc_movec,       sizeof(rom_tc_movec),       "MOVEC TC round-trip (cr=0x003)",    6 },
    { "urp_movec",      rom_urp_movec,      sizeof(rom_urp_movec),      "MOVEC URP round-trip (cr=0x806)",   6 },
    { "itt0_movec",     rom_itt0_movec,     sizeof(rom_itt0_movec),     "MOVEC ITT0 round-trip (cr=0x004)",  6 },
    { "cinva_noop",     rom_cinva_noop,     sizeof(rom_cinva_noop),     "CINVA IC: no-op, sentinel preserved", 4 },
    { "move16_copy",    rom_move16_copy,    sizeof(rom_move16_copy),    "MOVE16 (A0)+,(A1)+: 16-byte copy",  8 },
    { "fmove_rtrip",    rom_fmove_rtrip,    sizeof(rom_fmove_rtrip),    "FMOVE.L round-trip via FP0",        10 },
    { "fsave_frestore", rom_fsave_frestore, sizeof(rom_fsave_frestore), "FSAVE/FRESTORE: no-crash stub",      5 },
};

#define NUM_TESTS (sizeof(tests) / sizeof(tests[0]))

/* ------------------------------------------------------------------ */
/* Pass/fail criteria                                                  */
/* ------------------------------------------------------------------ */

static int check_result(size_t idx)
{
    switch (idx) {
    case 0: return cpu.d[1] == 0x55;          /* MOVEC TC round-trip */
    case 1: return cpu.d[1] == 0x2000;        /* MOVEC URP round-trip */
    case 2: return cpu.d[1] == 0x77;          /* MOVEC ITT0 round-trip */
    case 3: return cpu.d[0] == 99;            /* CINVA: sentinel unchanged */
    case 4: return cpu.d[0] == 0xDEADBEEF;   /* MOVE16: pattern copied */
    case 5: return cpu.d[1] == 42;            /* FMOVE.L round-trip */
    case 6: return cpu.d[0] == 77;            /* FSAVE/FRESTORE: sentinel unchanged */
    default: return 0;
    }
}

/* ------------------------------------------------------------------ */
/* Runner                                                              */
/* ------------------------------------------------------------------ */

int run_68040_tests(void)
{
    int failed = 0;

    printf("Running 68040 tests...\n");
    fflush(stdout);

    /* Switch to 68040 mode for this suite. */
    cpu_init(CPU_MODEL_68040);

    for (size_t i = 0; i < NUM_TESTS; i++) {
        const test68040_t *t = &tests[i];

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

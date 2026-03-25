/*
 * 68020-specific internal tests — Phase 1: full extension word EA.
 *
 * The 68020 extends the indexed addressing mode (mode 6) with two improvements:
 *
 *   1. Brief extension words (bit 8 = 0) now honour the SCALE field (bits 10-9),
 *      allowing the index register to be multiplied by 1, 2, 4, or 8 before
 *      being added to the base.  On 68000/010 those bits are always zero, so
 *      the shift is a harmless no-op.
 *
 *   2. Full extension words (bit 8 = 1) introduce a more flexible format:
 *      the base register and index register can each be suppressed independently,
 *      and the displacement can be 0, 16, or 32 bits wide.
 *
 * Each ROM follows the same setup convention as the 68010 tests:
 *   - Reset vector at 0x00 (SP = 0x1000) and 0x04 (PC = 0x10)
 *   - Code starts at 0x10
 *   - Halts with BRA.S . once the work is done
 */

#include "tests_68020.h"
#include "cpu.h"
#include "cpu_internal.h"
#include "memory.h"
#include <stdio.h>

/* ------------------------------------------------------------------ */
/* Test ROMs                                                           */
/* ------------------------------------------------------------------ */

/*
 * Test 1: Brief extension word with long-word index scaled by 4
 *
 *   MOVEA.L #0x1000, A0    ; A0 = base address
 *   MOVEQ   #5, D1         ; D1 = index value 5
 *   LEA     (0, A0, D1.L*4), A2   ; A2 = A0 + (D1 << 2) + 0
 *   BRA.S   .              ; halt
 *
 * Extension word breakdown (brief, bit 8 = 0):
 *   bit 15   = 0     D1 is a data register
 *   bits14-12= 001   register D1
 *   bit 11   = 1     use full 32-bit (long) index
 *   bits10-9 = 10    scale = 2, so index is shifted left 2 (× 4)
 *   bit 8    = 0     brief format
 *   bits 7-0 = 0x00  8-bit signed displacement = 0
 *   --------------------------------
 *   = 0b 0001_1100_0000_0000 = 0x1C00
 *
 * Expected: A2 = 0x1000 + (5 << 2) + 0 = 0x1014
 */
static const uint8_t rom_brief_scale[] = {
    0x00, 0x00, 0x10, 0x00,   /* SP = 0x1000 */
    0x00, 0x00, 0x00, 0x10,   /* PC = 0x0010 */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  /* padding */
    /* 0x10: MOVEA.L #0x1000, A0  (opcode 0x207C, then 32-bit immediate) */
    0x20, 0x7C, 0x00, 0x00, 0x10, 0x00,
    /* 0x16: MOVEQ #5, D1  (loads the small constant 5 into D1 in one word) */
    0x72, 0x05,
    /* 0x18: LEA (0, A0, D1.L*4), A2  (opcode 0x45F0, ext word 0x1C00) */
    0x45, 0xF0, 0x1C, 0x00,
    /* 0x1C: BRA.S .  (infinite loop — signals end of test) */
    0x60, 0xFE,
};

/*
 * Test 2: Brief extension word with word index scaled by 2
 *
 *   MOVEA.L #0x1000, A0    ; A0 = base address
 *   MOVEQ   #4, D0         ; D0 = index value 4
 *   LEA     (0x10, A0, D0.W*2), A1  ; A1 = A0 + sign_extend(D0.W) * 2 + 0x10
 *   BRA.S   .
 *
 * Extension word (brief, bit 8 = 0):
 *   bit 15   = 0     D0 is a data register
 *   bits14-12= 000   register D0
 *   bit 11   = 0     use lower 16-bit (word) index, sign-extended to 32
 *   bits10-9 = 01    scale = 1, so index is shifted left 1 (× 2)
 *   bit 8    = 0     brief format
 *   bits 7-0 = 0x10  8-bit signed displacement = 16
 *   --------------------------------
 *   = 0b 0000_0010_0001_0000 = 0x0210
 *
 * Expected: A1 = 0x1000 + (4 << 1) + 0x10 = 0x1018
 */
static const uint8_t rom_brief_word_scale[] = {
    0x00, 0x00, 0x10, 0x00,
    0x00, 0x00, 0x00, 0x10,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    /* 0x10: MOVEA.L #0x1000, A0 */
    0x20, 0x7C, 0x00, 0x00, 0x10, 0x00,
    /* 0x16: MOVEQ #4, D0 */
    0x70, 0x04,
    /* 0x18: LEA (0x10, A0, D0.W*2), A1  (opcode 0x43F0, ext word 0x0210) */
    0x43, 0xF0, 0x02, 0x10,
    /* 0x1C: BRA.S . */
    0x60, 0xFE,
};

/*
 * Test 3: Full extension word with 16-bit base displacement
 *
 *   MOVEA.L #0x20, A0      ; A0 = 0x20  (points into this ROM)
 *   MOVEQ   #0, D0         ; D0 = 0  (zero index, so it contributes nothing)
 *   MOVE.L  (0x10, A0, D0.L*1), D1  ; read long-word from [A0 + D0 + 0x10]
 *   BRA.S   .
 *   (data at 0x30: 0xCAFEBABE)
 *
 * Full extension word for D0.L*1 with 16-bit base displacement:
 *   bit 15   = 0     D0 is a data register
 *   bits14-12= 000   register D0
 *   bit 11   = 1     use full 32-bit (long) index
 *   bits10-9 = 00    scale = 0  (× 1, no shift)
 *   bit 8    = 1     FULL extension word
 *   bit 7    = 0     BS=0: don't suppress base (use A0 normally)
 *   bit 6    = 0     IS=0: don't suppress index (use D0 normally)
 *   bits 5-4 = 10    BD size = word: fetch a 16-bit signed base displacement
 *   bit 3    = 0     (reserved)
 *   bit 2    = 0     no memory-indirect
 *   bits 1-0 = 00    OD = none (plain register-indirect, no outer displacement)
 *   --------------------------------
 *   = 0b 0000_1001_0010_0000 = 0x0920
 *   followed by BD word = 0x0010  (decimal 16)
 *
 * Effective address: A0 + D0 + BD = 0x20 + 0 + 0x10 = 0x30
 * Value at 0x30: 0xCAFEBABE
 *
 * Expected: D1 = 0xCAFEBABE
 */
static const uint8_t rom_full_ext[] = {
    0x00, 0x00, 0x10, 0x00,   /* SP = 0x1000 */
    0x00, 0x00, 0x00, 0x10,   /* PC = 0x0010 */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  /* padding */
    /* 0x10: MOVEA.L #0x20, A0 */
    0x20, 0x7C, 0x00, 0x00, 0x00, 0x20,
    /* 0x16: MOVEQ #0, D0 */
    0x70, 0x00,
    /* 0x18: MOVE.L (0x10, A0, D0.L*1), D1
     *   opcode  = 0x2230  (MOVE.L, src=mode6/A0, dst=D1)
     *   ext     = 0x0920  (full ext, D0.L, scale×1, BD=word)
     *   BD word = 0x0010  (displacement = 16) */
    0x22, 0x30, 0x09, 0x20, 0x00, 0x10,
    /* 0x1E: BRA.S . */
    0x60, 0xFE,
    /* 0x20-0x2F: 16 bytes of padding so data lands at 0x30 */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    /* 0x30: data word read by the MOVE.L above */
    0xCA, 0xFE, 0xBA, 0xBE,
};

/*
 * Test 4: BRA.L — 32-bit branch displacement
 *
 * On 68020+, when the low byte of a Bcc/BRA/BSR opcode is 0xFF, the next
 * four bytes are a signed 32-bit displacement rather than an 8-bit -1.
 * This allows branches to any address in the 32-bit address space.
 *
 * Layout:
 *   0x10: BRA.L  disp=0x0E  → target = (0x10+2) + 0x0E = 0x20
 *   0x16: MOVEQ #0xFF, D0   ← must NOT be reached (branch skips over it)
 *   0x18: BRA.S .           ← also not reached
 *   0x1A-0x1F: padding
 *   0x20: MOVEQ #1, D0      ← branch lands here
 *   0x22: BRA.S .           ← halt
 *
 * Opcode 0x60FF = BRA with low byte 0xFF (the 32-bit displacement marker).
 * Followed by 32-bit displacement 0x0000000E (14 decimal).
 *
 * Expected: D0 = 1  (proves the branch was taken to 0x20, not fallen through)
 */
static const uint8_t rom_bra_l[] = {
    0x00, 0x00, 0x10, 0x00,   /* SP = 0x1000 */
    0x00, 0x00, 0x00, 0x10,   /* PC = 0x0010 */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  /* padding */
    /* 0x10: BRA.L  — opcode 0x60FF, then 32-bit displacement */
    0x60, 0xFF, 0x00, 0x00, 0x00, 0x0E,
    /* 0x16: MOVEQ #0xFF, D0  — NOT reached if branch was taken */
    0x70, 0xFF,
    /* 0x18: BRA.S .          — NOT reached */
    0x60, 0xFE,
    /* 0x1A-0x1F: padding */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    /* 0x20: MOVEQ #1, D0     — branch should land here */
    0x70, 0x01,
    /* 0x22: BRA.S .          — halt */
    0x60, 0xFE,
};

/*
 * Test 5: MULU.L — 32-bit unsigned multiply
 *
 *   MOVEQ  #6, D0          ; D0 = 6  (multiplicand, held in Dl)
 *   MOVEQ  #7, D1          ; D1 = 7  (source operand, via register EA)
 *   MULU.L D1, D0          ; D0 = D0 * D1 = 42  (32-bit result)
 *   BRA.S  .
 *
 * MULU.L opcode: 0x4C00 | EA(D1)
 *   EA for D1 = mode 0, reg 1 → bits 5-0 = 0b000001 = 0x01
 *   Opcode = 0x4C01
 *
 * Extension word (32-bit unsigned, Dl=D0):
 *   bits 14-12 = 000  (Dh, unused in 32-bit mode)
 *   bit  11    = 0    (unsigned)
 *   bit  9     = 0    (32-bit result)
 *   bits  2-0  = 000  (Dl = D0)
 *   = 0x0000
 *
 * Expected: D0 = 42
 */
static const uint8_t rom_mulu_l[] = {
    0x00, 0x00, 0x10, 0x00,   /* SP = 0x1000 */
    0x00, 0x00, 0x00, 0x10,   /* PC = 0x0010 */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  /* padding */
    /* 0x10: MOVEQ #6, D0 */
    0x70, 0x06,
    /* 0x12: MOVEQ #7, D1 */
    0x72, 0x07,
    /* 0x14: MULU.L D1, D0  (opcode 0x4C01, ext 0x0000) */
    0x4C, 0x01, 0x00, 0x00,
    /* 0x18: BRA.S . */
    0x60, 0xFE,
};

/*
 * Test 6: MULS.L — 32-bit signed multiply with immediate source
 *
 *   MOVEQ  #5, D0            ; D0 = 5  (multiplicand)
 *   MULS.L #0xFFFFFFFD, D0   ; D0 = 5 * (-3) = -15 = 0xFFFFFFF1
 *   BRA.S  .
 *
 * MULS.L #imm, D0 opcode: 0x4C00 | EA(#imm)
 *   EA for #imm = mode 7, reg 4 → bits 5-0 = 0b111100 = 0x3C
 *   Opcode = 0x4C3C
 *
 * Extension word (32-bit signed, Dl=D0):
 *   bit 11 = 1  (signed)
 *   bit  9 = 0  (32-bit result)
 *   bits 2-0 = 000  (Dl = D0)
 *   = 0x0800
 *
 * Followed by 32-bit immediate 0xFFFFFFFD (= -3 in two's complement).
 *
 * Expected: D0 = 0xFFFFFFF1  (= -15)
 */
static const uint8_t rom_muls_l[] = {
    0x00, 0x00, 0x10, 0x00,
    0x00, 0x00, 0x00, 0x10,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    /* 0x10: MOVEQ #5, D0 */
    0x70, 0x05,
    /* 0x12: MULS.L #0xFFFFFFFD, D0  (opcode 0x4C3C, ext 0x0800, imm 0xFFFFFFFD) */
    0x4C, 0x3C, 0x08, 0x00, 0xFF, 0xFF, 0xFF, 0xFD,
    /* 0x1A: BRA.S . */
    0x60, 0xFE,
};

/*
 * Test 7: DIVU.L — 64-bit unsigned divide
 *
 *   MOVEQ  #100, D0       ; D0 = 100  (low 32 bits of 64-bit dividend)
 *   MOVEQ  #0,   D1       ; D1 = 0    (high 32 bits of 64-bit dividend)
 *   MOVEQ  #7,   D2       ; D2 = 7    (divisor, via register EA)
 *   DIVU.L D2, D1:D0      ; 64-bit dividend D1:D0 = 100 / 7
 *   BRA.S  .
 *
 * DIVU.L D2, D1:D0 opcode: 0x4C40 | EA(D2)
 *   EA for D2 = mode 0, reg 2 → bits 5-0 = 0b000010 = 0x02
 *   Opcode = 0x4C42
 *
 * Extension word (unsigned, Dr=D1, Dq=D0):
 *   bits 14-12 = 001  (Dr = D1, holds HIGH 32 bits of dividend, receives remainder)
 *   bit  11    = 0    (unsigned)
 *   bits  2-0  = 000  (Dq = D0, holds LOW 32 bits of dividend, receives quotient)
 *   = 0x1000
 *
 * Since Dr (D1) != Dq (D0), the full 64-bit divide path is used:
 *   64-bit dividend = (D1 << 32) | D0 = (0 << 32) | 100 = 100
 *   100 / 7 = quotient 14, remainder 2
 *
 * Expected: D0 = 14 (quotient), D1 = 2 (remainder)
 */
static const uint8_t rom_divu_l[] = {
    0x00, 0x00, 0x10, 0x00,
    0x00, 0x00, 0x00, 0x10,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    /* 0x10: MOVEQ #100, D0  (100 = 0x64) */
    0x70, 0x64,
    /* 0x12: MOVEQ #0, D1 */
    0x72, 0x00,
    /* 0x14: MOVEQ #7, D2 */
    0x74, 0x07,
    /* 0x16: DIVU.L D2, D1:D0  (opcode 0x4C42, ext 0x1000) */
    0x4C, 0x42, 0x10, 0x00,
    /* 0x1A: BRA.S . */
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
} test68020_t;

static const test68020_t tests[] = {
    { "brief_scale",      rom_brief_scale,      sizeof(rom_brief_scale),      "Brief ext: long index × 4",            10 },
    { "brief_word_scale", rom_brief_word_scale, sizeof(rom_brief_word_scale), "Brief ext: word index × 2 + disp8",    10 },
    { "full_ext_bd16",    rom_full_ext,         sizeof(rom_full_ext),         "Full ext: word base displacement",      10 },
    { "bra_l",            rom_bra_l,            sizeof(rom_bra_l),            "BRA.L 32-bit displacement",             5  },
    { "mulu_l",           rom_mulu_l,           sizeof(rom_mulu_l),           "MULU.L 32-bit unsigned multiply",       5  },
    { "muls_l",           rom_muls_l,           sizeof(rom_muls_l),           "MULS.L 32-bit signed multiply",         5  },
    { "divu_l",           rom_divu_l,           sizeof(rom_divu_l),           "DIVU.L 64-bit unsigned divide",         7  },
};

#define NUM_TESTS (sizeof(tests) / sizeof(tests[0]))

/* ------------------------------------------------------------------ */
/* Pass/fail criteria                                                  */
/* ------------------------------------------------------------------ */

static int check_result(size_t idx)
{
    switch (idx) {
    case 0: return cpu.a[2] == 0x1014;          /* A2 = 0x1000 + (5<<2) + 0 */
    case 1: return cpu.a[1] == 0x1018;          /* A1 = 0x1000 + (4<<1) + 0x10 */
    case 2: return cpu.d[1] == 0xCAFEBABE;      /* D1 = value at [A0 + D0 + BD] */
    case 3: return cpu.d[0] == 1;               /* D0 = 1: landed at 0x20, not 0x16 */
    case 4: return cpu.d[0] == 42;              /* MULU.L: 6 × 7 = 42 */
    case 5: return cpu.d[0] == 0xFFFFFFF1;      /* MULS.L: 5 × -3 = -15 */
    case 6: return cpu.d[0] == 14 && cpu.d[1] == 2;  /* DIVU.L: 100 / 7 = q14 r2 */
    default: return 0;
    }
}

/* ------------------------------------------------------------------ */
/* Runner                                                              */
/* ------------------------------------------------------------------ */

int run_68020_tests(void)
{
    int failed = 0;

    printf("Running 68020 tests...\n");
    fflush(stdout);

    /* Switch to 68020 mode for this suite. */
    cpu_init(CPU_MODEL_68020);

    for (size_t i = 0; i < NUM_TESTS; i++) {
        const test68020_t *t = &tests[i];

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
            printf("  %-20s PASS\n", t->name);
        } else {
            printf("  %-20s FAIL\n", t->name);
            failed = 1;
        }
    }

    /* Restore to 68000 so subsequent tests are not affected. */
    cpu_init(CPU_MODEL_68000);

    return failed;
}

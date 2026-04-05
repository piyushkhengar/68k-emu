/*
 * 68080-specific internal tests (Apollo AC68080 — AMMX SIMD coprocessor).
 *
 * The 68080 is code-compatible with 68060 and adds:
 *   - AMMX: Apollo MultiMedia eXtension, coprocessor ID=7 (opcodes 0xFE00-0xFFFF)
 *   - 24 × 64-bit SIMD registers E0-E23 (cpu.e[0..23])
 *
 * AMMX instruction encoding (derived from vasm Apollo Core opcodes.h):
 *
 *   First opword:  1111 111 A B D VEA[5:0]
 *     bits 15-9 = 1111111 (F-line + cpID=7)
 *     bit  8 (A) = source-1 register bit 4 (0 for D0-D7/E0-E7, 1 for E8-E23)
 *     bit  7 (B) = source-2 register bit 4
 *     bit  6 (D) = destination  register bit 4
 *     bits 4-0   = source-1 register number low bits (0-7=D0-D7, 8-15=E0-E7)
 *
 *   Extension word (2nd word):
 *     bits 15-11 = destination register bits 3-0 (bit 4 from D-bit, above)
 *     bits 10-6  = source-2   register bits 3-0 (bit 4 from B-bit, above)
 *     bits  5-0  = opmode (6-bit instruction selector)
 *
 *   Register numbering: 0-7 = D0-D7, 8-31 = E0-E23
 *
 * AMMX opmode assignments (from vasm Apollo Core opcodes.h):
 *   0x01 LOAD     0x08 PAND     0x09 POR     0x0A PEOR    0x0B PANDN
 *   0x10 PADDB    0x11 PADDW    0x12 PSUBB   0x13 PSUBW
 *   0x20 PCMPEQB  0x30 PMINSB  0x34 PMAXSB  ...
 *
 * Binary encoding examples used in tests:
 *
 *   PADDB D0, D1, D2:
 *     A=0, B=0, D=0, src1=D0 → first word = 0xFE00
 *     ext = (dst=2 << 11) | (src2=1 << 6) | opmode=0x10 = 0x1050
 *     bytes: FE 00 10 50
 *
 *   PEOR D0, D0, D1  (self-XOR → zero):
 *     A=0, B=0, D=0, src1=D0 → first word = 0xFE00
 *     ext = (dst=1 << 11) | (src2=0 << 6) | opmode=0x0A = 0x080A
 *     bytes: FE 00 08 0A
 *
 *   PADDB D0, D1, E0  (result in E0 = register 8):
 *     A=0, B=0, D=0, src1=D0 → first word = 0xFE00
 *     dst=8, dst_low4=8 → (8 << 11) = 0x4000
 *     ext = (8 << 11) | (1 << 6) | 0x10 = 0x4000 | 0x40 | 0x10 = 0x4050
 *     bytes: FE 00 40 50
 *
 * Setup convention (same as all other model test files):
 *   - Reset vector at 0x00 (SP = 0x1000) and 0x04 (PC = 0x10)
 *   - Code starts at 0x10
 *   - Halts with BRA.S . or cpu.halted == 1
 */

#include "tests_68080.h"
#include "cpu.h"
#include "cpu_internal.h"
#include "memory.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>

/* ------------------------------------------------------------------ */
/* Test ROMs                                                           */
/* ------------------------------------------------------------------ */

/*
 * Test 1: LPSTOP still works on 68080 (inherited from 68060)
 *
 *   MOVEQ #77, D0
 *   LPSTOP #0x2700   ; load SR=0x2700, halt
 *
 * Expected: cpu.d[0] == 77 && cpu.halted == 1
 */
static const uint8_t rom_lpstop_68080[] = {
    0x00, 0x00, 0x10, 0x00,   /* SP = 0x1000 */
    0x00, 0x00, 0x00, 0x10,   /* PC = 0x10   */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  /* padding */
    /* 0x10: MOVEQ #77, D0 */
    0x70, 0x4D,
    /* 0x12: LPSTOP #0x2700  (opword 0xF800, ext 0x2700) */
    0xF8, 0x00, 0x27, 0x00,
};

/*
 * Test 2: PADDB — packed byte add D0, D1 → D2
 *
 *   MOVE.L #0x01020304, D0    ; packed bytes {4,3,2,1} low-to-high
 *   MOVE.L #0x04030201, D1    ; packed bytes {1,2,3,4}
 *   PADDB  D0, D1, D2         ; FE 00 10 50
 *
 * Expected: D2 = 0x05050505  (each byte pair sums to 5)
 */
static const uint8_t rom_paddb[] = {
    0x00, 0x00, 0x10, 0x00,
    0x00, 0x00, 0x00, 0x10,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    /* 0x10: MOVE.L #0x01020304, D0  (0x203C + 4 bytes) */
    0x20, 0x3C, 0x01, 0x02, 0x03, 0x04,
    /* 0x16: MOVE.L #0x04030201, D1  (0x223C + 4 bytes) */
    0x22, 0x3C, 0x04, 0x03, 0x02, 0x01,
    /* 0x1C: PADDB D0, D1, D2
     *   first word: 0xFE00 (src1=D0, A=0, B=0, D=0, vea=0)
     *   ext word:   0x1050 = (dst=2 << 11) | (src2=1 << 6) | opmode=0x10 */
    0xFE, 0x00, 0x10, 0x50,
    /* 0x20: BRA.S . */
    0x60, 0xFE,
};

/*
 * Test 3: PEOR — packed XOR D0, D0, D1  (self-XOR → zero)
 *
 *   MOVE.L #0xDEADBEEF, D0   ; arbitrary non-zero value
 *   PEOR   D0, D0, D1        ; FE 00 08 0A
 *
 * Expected: D1 = 0x00000000  (any value XOR itself = 0)
 */
static const uint8_t rom_peor_zero[] = {
    0x00, 0x00, 0x10, 0x00,
    0x00, 0x00, 0x00, 0x10,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    /* 0x10: MOVE.L #0xDEADBEEF, D0 */
    0x20, 0x3C, 0xDE, 0xAD, 0xBE, 0xEF,
    /* 0x16: PEOR D0, D0, D1
     *   first word: 0xFE00 (src1=D0)
     *   ext word:   0x080A = (dst=1 << 11) | (src2=D0=0 << 6) | opmode=0x0A */
    0xFE, 0x00, 0x08, 0x0A,
    /* 0x1A: BRA.S . */
    0x60, 0xFE,
};

/*
 * Test 4: PADDB with E register destination (E0 = AMMX register 8)
 *
 *   MOVE.L #0x01010101, D0   ; all bytes = 1
 *   MOVE.L #0x02020202, D1   ; all bytes = 2
 *   PADDB  D0, D1, E0        ; FE 00 40 50
 *
 * Expected: cpu.e[0] == 0x0000000003030303ULL
 *   (upper 32 bits = 0 since D0/D1 are 32-bit zero-extended; each byte = 1+2 = 3)
 */
static const uint8_t rom_paddb_ereg[] = {
    0x00, 0x00, 0x10, 0x00,
    0x00, 0x00, 0x00, 0x10,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    /* 0x10: MOVE.L #0x01010101, D0 */
    0x20, 0x3C, 0x01, 0x01, 0x01, 0x01,
    /* 0x16: MOVE.L #0x02020202, D1 */
    0x22, 0x3C, 0x02, 0x02, 0x02, 0x02,
    /* 0x1C: PADDB D0, D1, E0
     *   first word: 0xFE00 (src1=D0, A=0, B=0, D=0)
     *   ext word:   0x4050 = (dst=E0=8 → low4=8 → 8<<11=0x4000) | (src2=1<<6) | 0x10 */
    0xFE, 0x00, 0x40, 0x50,
    /* 0x20: BRA.S . */
    0x60, 0xFE,
};

/*
 * Test 5: PSUBB — packed byte subtract  (locks in D = B − A operand order)
 *
 *   MOVE.L #0x03030303, D0   ; A (src1)
 *   MOVE.L #0x05050505, D1   ; B (src2)
 *   PSUBB  D0, D1, D2        ; D2 = B − A = 0x02020202
 *
 * Encoding:
 *   PSUBB D0,D1,D2 — opmode=0x12
 *   first word: 0xFE00 (A=0,B=0,D=0, src1=D0)
 *   ext word:   (2<<11)|(1<<6)|0x12 = 0x1052
 */
static const uint8_t rom_psubb[] = {
    0x00, 0x00, 0x10, 0x00,
    0x00, 0x00, 0x00, 0x10,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    /* 0x10: MOVE.L #0x03030303, D0 */
    0x20, 0x3C, 0x03, 0x03, 0x03, 0x03,
    /* 0x16: MOVE.L #0x05050505, D1 */
    0x22, 0x3C, 0x05, 0x05, 0x05, 0x05,
    /* 0x1C: PSUBB D0, D1, D2  →  D2 = B−A = 0x02020202 */
    0xFE, 0x00, 0x10, 0x52,
    /* 0x20: BRA.S . */
    0x60, 0xFE,
};

/*
 * Test 6: PMUL88 — fractional 8×8 multiply (verifies the >>8 shift)
 *
 *   MOVE.L #0x00100010, D0   ; two word lanes, each low byte = 0x10 (16)
 *   MOVE.L #0x00100010, D1   ; same
 *   PMUL88 D0, D1, D2        ; D2 = (16*16)>>8 = 1 per lane = 0x00010001
 *
 *   Without the >>8 shift the result would be 0x01000100 — a detectable difference.
 *
 * Encoding:
 *   PMUL88 — opmode=0x18
 *   first word: 0xFE00
 *   ext word:   (2<<11)|(1<<6)|0x18 = 0x1058
 */
static const uint8_t rom_pmul88[] = {
    0x00, 0x00, 0x10, 0x00,
    0x00, 0x00, 0x00, 0x10,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    /* 0x10: MOVE.L #0x00100010, D0 */
    0x20, 0x3C, 0x00, 0x10, 0x00, 0x10,
    /* 0x16: MOVE.L #0x00100010, D1 */
    0x22, 0x3C, 0x00, 0x10, 0x00, 0x10,
    /* 0x1C: PMUL88 D0, D1, D2 */
    0xFE, 0x00, 0x10, 0x58,
    /* 0x20: BRA.S . */
    0x60, 0xFE,
};

/*
 * Test 7: Memory LOAD + STORE round-trip
 *
 *   Build E0 = 0x0000000003030303 via PADDB (D0=0x01010101, D1=0x02020202).
 *   STORE E0, (A0) — write 8 bytes to address 0x100.
 *   LOAD  (A0), E1 — read them back.
 *
 *   Expected: cpu.e[1] == 0x0000000003030303ULL
 *
 * STORE E0,(A0) encoding:
 *   VEA = (A0) = mode 2, reg 0 → op bits 5:0 = 0b010000 = 0x10
 *   first word: 0xFE10  (A=0,B=0,D=0, VEA=0x10)
 *   ext word:   (dst=E0=8, low4=8 → 8<<11=0x4000) | opmode=0x04 → 0x4004
 *
 * LOAD (A0),E1 encoding:
 *   first word: 0xFE10
 *   ext word:   (dst=E1=9, low4=9 → 9<<11=0x4800) | opmode=0x01 → 0x4801
 */
static const uint8_t rom_load_store[] = {
    0x00, 0x00, 0x10, 0x00,
    0x00, 0x00, 0x00, 0x10,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    /* 0x10: MOVE.L #0x01010101, D0 */
    0x20, 0x3C, 0x01, 0x01, 0x01, 0x01,
    /* 0x16: MOVE.L #0x02020202, D1 */
    0x22, 0x3C, 0x02, 0x02, 0x02, 0x02,
    /* 0x1C: PADDB D0,D1,E0 → E0 = 0x0000000003030303 */
    0xFE, 0x00, 0x40, 0x50,
    /* 0x20: LEA 0x100.W, A0  (41F8 0100) */
    0x41, 0xF8, 0x01, 0x00,
    /* 0x24: STORE E0, (A0) */
    0xFE, 0x10, 0x40, 0x04,
    /* 0x28: LOAD  (A0), E1 */
    0xFE, 0x10, 0x48, 0x01,
    /* 0x2C: BRA.S . */
    0x60, 0xFE,
};

/*
 * Test 8: PMAXUW — unsigned word maximum (verifies 0x37 opmode)
 *
 *   MOVE.L #0x00010003, D0   ; words {3, 1}
 *   MOVE.L #0x00020002, D1   ; words {2, 2}
 *   PMAXUW D0, D1, D2        ; max per lane: {3, 2} = 0x00030002
 *
 *   PMAXUW opmode=0x37, first=0xFE00, ext=(2<<11)|(1<<6)|0x37=0x1077
 */
static const uint8_t rom_pmaxuw[] = {
    0x00, 0x00, 0x10, 0x00,
    0x00, 0x00, 0x00, 0x10,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    /* 0x10: MOVE.L #0x00010003, D0 */
    0x20, 0x3C, 0x00, 0x01, 0x00, 0x03,
    /* 0x16: MOVE.L #0x00020002, D1 */
    0x22, 0x3C, 0x00, 0x02, 0x00, 0x02,
    /* 0x1C: PMAXUW D0, D1, D2  →  D2 = 0x00030002 */
    0xFE, 0x00, 0x10, 0x77,
    /* 0x20: BRA.S . */
    0x60, 0xFE,
};

/*
 * Test 9: PACKUSWB — pack signed words to saturated bytes
 *
 *   MOVE.L #0x00FF0100, D0   ; words {0x0100=256 → sat 255, 0x00FF=255}
 *   MOVE.L #0x00010000, D1   ; words {0x0000=0,   0x0001=1}
 *   PACKUSWB D0, D1, D2      ; bytes from a: {255,255}, from b: {0,1}
 *                             ; D2 = 0x000100FF_FFFF (low 4 bytes) = 0x000100FFFF
 *                             ; layout: b[1],b[0],a[1],a[0] in bytes 3..0
 *                             ;       = {0x00, 0x01, 0xFF, 0xFF} → 0x000100FFFF? No…
 *
 * Simpler: use values that pack cleanly.
 *   D0 = 0x00020001  → words {2, 1} → bytes {2, 1}
 *   D1 = 0x00040003  → words {4, 3} → bytes {4, 3}
 *   D2 = 0x04030201 (b[1],b[0],a[1],a[0])
 *
 *   opmode=0x06, ext=(2<<11)|(1<<6)|0x06=0x1046
 */
static const uint8_t rom_packuswb[] = {
    0x00, 0x00, 0x10, 0x00,
    0x00, 0x00, 0x00, 0x10,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    /* 0x10: MOVE.L #0x00020001, D0 */
    0x20, 0x3C, 0x00, 0x02, 0x00, 0x01,
    /* 0x16: MOVE.L #0x00040003, D1 */
    0x22, 0x3C, 0x00, 0x04, 0x00, 0x03,
    /* 0x1C: PACKUSWB D0, D1, E0
     *   dst=E0=8: D-bit=0, dst_low4=8 → ext = (8<<11)|(1<<6)|0x06 = 0x4046 */
    0xFE, 0x00, 0x40, 0x46,
    /* 0x20: BRA.S . */
    0x60, 0xFE,
};

/*
 * Test 10: PADDB from memory EA — arithmetic with memory source operand
 *
 *   Store 8 bytes {1,1,1,1,1,1,1,1} at address 0x100.
 *   MOVE.L #0x02020202, D1   ; src2 = 4 bytes of value 2 (zero-extended to 64-bit)
 *   LEA 0x100.W, A0
 *   PADDB (A0), D1, D2       ; D2 = mem[0x100] + D1 = 0x03030303
 *
 *   PADDB with memory src1 (A0):
 *     vea_mode=2, vea_reg=0 → op bits 5:0 = 0b010_000 = 0x10
 *     first word: 0xFE10 (A=0, B=0, D=0, VEA=mode2,reg0)
 *     ext: (dst=D2=2)<<11 | (src2=D1=1)<<6 | opmode=0x10
 *        = 0x1000 | 0x0040 | 0x10 = 0x1050
 *
 * ROM places data at 0x30 inline (8 bytes of 0x01).
 */
static const uint8_t rom_paddb_mem[] = {
    0x00, 0x00, 0x10, 0x00,
    0x00, 0x00, 0x00, 0x10,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    /* 0x10: MOVE.L #0x02020202, D1 */
    0x22, 0x3C, 0x02, 0x02, 0x02, 0x02,
    /* 0x16: LEA 0x30.W, A0 */
    0x41, 0xF8, 0x00, 0x30,
    /* 0x1A: PADDB (A0), D1, D2  →  D2 = mem[0x30] + D1 */
    0xFE, 0x10, 0x10, 0x50,
    /* 0x1E: BRA.S . */
    0x60, 0xFE,
    /* 0x20-0x2F: padding */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    /* 0x30: data — 8 bytes of 0x01 */
    0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
};

/*
 * Test 11: TRANSHI — 4×4 word matrix transpose, upper word pair.
 *   Load E0-E3 from memory:
 *     E0=0x0001000200030004, E1=0x0005000600070008
 *     E2=0x0009000A000B000C, E3=0x000D000E000F0010
 *   TRANSHI E0-E3, E4:E5:
 *     E4 = [w0_E0, w0_E1, w0_E2, w0_E3] = [0x0004,0x0008,0x000C,0x0010]
 *        = 0x0010000C00080004
 *     E5 = [w1_E0, w1_E1, w1_E2, w1_E3] = [0x0003,0x0007,0x000B,0x000F]
 *        = 0x000F000B00070003
 *
 *   MOVEA.L #0x2C,A0 = 0x207C,0x0000002C
 *   LOAD (A0)+,E0: VEA=(A0)+=mode3,reg0→0x18; first=0xFE18; ext=(8<<11)|0x01=0x4001
 *   LOAD (A0)+,E1: ext=(9<<11)|0x01=0x4801
 *   LOAD (A0)+,E2: ext=(10<<11)|0x01=0x5001
 *   LOAD (A0)+,E3: ext=(11<<11)|0x01=0x5801
 *   TRANSHI E0-E3,E4:E5: src1=E0(reg8)→VEA=mode1,reg0=0x08; first=0xFE08
 *     dst=E4(reg12); ext=(12<<11)|0x02=0x6002
 */
static const uint8_t rom_transhi[] = {
    0x00, 0x00, 0x10, 0x00,
    0x00, 0x00, 0x00, 0x10,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    /* 0x10: MOVEA.L #0x2C, A0 */
    0x20, 0x7C, 0x00, 0x00, 0x00, 0x2C,
    /* 0x16: LOAD (A0)+, E0 */
    0xFE, 0x18, 0x40, 0x01,
    /* 0x1A: LOAD (A0)+, E1 */
    0xFE, 0x18, 0x48, 0x01,
    /* 0x1E: LOAD (A0)+, E2 */
    0xFE, 0x18, 0x50, 0x01,
    /* 0x22: LOAD (A0)+, E3 */
    0xFE, 0x18, 0x58, 0x01,
    /* 0x26: TRANSHI E0-E3, E4:E5 */
    0xFE, 0x08, 0x60, 0x02,
    /* 0x2A: BRA.S . */
    0x60, 0xFE,
    /* 0x2C: data for E0-E3 (big-endian 8 bytes each) */
    0x00,0x01,0x00,0x02,0x00,0x03,0x00,0x04,  /* E0=0x0001000200030004 */
    0x00,0x05,0x00,0x06,0x00,0x07,0x00,0x08,  /* E1=0x0005000600070008 */
    0x00,0x09,0x00,0x0A,0x00,0x0B,0x00,0x0C,  /* E2=0x0009000A000B000C */
    0x00,0x0D,0x00,0x0E,0x00,0x0F,0x00,0x10,  /* E3=0x000D000E000F0010 */
};

/*
 * Test 12: TRANSLO — 4×4 word matrix transpose, lower word pair.
 *   Same data load as TRANSHI test; TRANSLO E0-E3, E4:E5:
 *     E4 = [w2_E0, w2_E1, w2_E2, w2_E3] = [0x0002,0x0006,0x000A,0x000E]
 *        = 0x000E000A00060002
 *     E5 = [w3_E0, w3_E1, w3_E2, w3_E3] = [0x0001,0x0005,0x0009,0x000D]
 *        = 0x000D000900050001
 *
 *   TRANSLO E0-E3,E4:E5: first=0xFE08; ext=(12<<11)|0x03=0x6003
 */
static const uint8_t rom_translo[] = {
    0x00, 0x00, 0x10, 0x00,
    0x00, 0x00, 0x00, 0x10,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    /* 0x10: MOVEA.L #0x2C, A0 */
    0x20, 0x7C, 0x00, 0x00, 0x00, 0x2C,
    /* 0x16: LOAD (A0)+, E0 */
    0xFE, 0x18, 0x40, 0x01,
    /* 0x1A: LOAD (A0)+, E1 */
    0xFE, 0x18, 0x48, 0x01,
    /* 0x1E: LOAD (A0)+, E2 */
    0xFE, 0x18, 0x50, 0x01,
    /* 0x22: LOAD (A0)+, E3 */
    0xFE, 0x18, 0x58, 0x01,
    /* 0x26: TRANSLO E0-E3, E4:E5 */
    0xFE, 0x08, 0x60, 0x03,
    /* 0x2A: BRA.S . */
    0x60, 0xFE,
    /* 0x2C: data */
    0x00,0x01,0x00,0x02,0x00,0x03,0x00,0x04,
    0x00,0x05,0x00,0x06,0x00,0x07,0x00,0x08,
    0x00,0x09,0x00,0x0A,0x00,0x0B,0x00,0x0C,
    0x00,0x0D,0x00,0x0E,0x00,0x0F,0x00,0x10,
};

/*
 * Test 13: C2P — 8×8 bit-matrix transpose (chunky-to-planar).
 *   D0=0x00000001 → a=0x0000000000000001
 *   C2P: byte 0 bit 0 set → output byte 7 bit 7 set → E0=0x8000000000000000
 *   opmode=0x28, first=0xFE00, ext=(8<<11)|0x28=0x4028
 */
static const uint8_t rom_c2p[] = {
    0x00, 0x00, 0x10, 0x00,
    0x00, 0x00, 0x00, 0x10,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    /* 0x10: MOVE.L #1, D0 */
    0x20, 0x3C, 0x00, 0x00, 0x00, 0x01,
    /* 0x16: C2P D0,D0,E0 (only uses src1=a) */
    0xFE, 0x00, 0x40, 0x28,
    /* 0x1A: BRA.S . */
    0x60, 0xFE,
};

/*
 * Test 14: BSEL — ternary byte blend.
 *   D0=0xFFFFFFFF (a: source where mask=1)
 *   D1=0xF0F0F0F0 (b: mask — high nibbles select from a)
 *   D2=0           (initial dst, from reset)
 *   BSEL D0,D1,D2 → D2 = (a&b)|(d&~b) = 0xF0F0F0F0
 *   opmode=0x29, first=0xFE00, ext=(2<<11)|(1<<6)|0x29=0x1069
 */
static const uint8_t rom_bsel[] = {
    0x00, 0x00, 0x10, 0x00,
    0x00, 0x00, 0x00, 0x10,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    /* 0x10: MOVE.L #0xFFFFFFFF, D0 */
    0x20, 0x3C, 0xFF, 0xFF, 0xFF, 0xFF,
    /* 0x16: MOVE.L #0xF0F0F0F0, D1 */
    0x22, 0x3C, 0xF0, 0xF0, 0xF0, 0xF0,
    /* 0x1C: BSEL D0,D1→D2 (D2=0 from reset) */
    0xFE, 0x00, 0x10, 0x69,
    /* 0x20: BRA.S . */
    0x60, 0xFE,
};

/*
 * Test 15: PCMPGTB — packed signed byte greater-than (result = b > a).
 *   D0=3 (a), D1=5 (b): byte 0 → 5>3 → 0xFF; upper bytes → 0>0 → 0x00
 *   D2 = 0x000000FF
 *   opmode=0x2E, first=0xFE00, ext=(2<<11)|(1<<6)|0x2E=0x106E
 */
static const uint8_t rom_pcmpgtb[] = {
    0x00, 0x00, 0x10, 0x00,
    0x00, 0x00, 0x00, 0x10,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    /* 0x10: MOVE.L #3, D0 (a) */
    0x20, 0x3C, 0x00, 0x00, 0x00, 0x03,
    /* 0x16: MOVE.L #5, D1 (b) */
    0x22, 0x3C, 0x00, 0x00, 0x00, 0x05,
    /* 0x1C: PCMPGTB D0,D1→D2 */
    0xFE, 0x00, 0x10, 0x6E,
    /* 0x20: BRA.S . */
    0x60, 0xFE,
};

/*
 * Test 16: LSLQ — 64-bit logical shift left.
 *   D0=4 (shift count = a), D1=1 (value = b)
 *   LSLQ D0,D1,E0: E0 = 1<<4 = 0x0000000000000010
 *   opmode=0x38, first=0xFE00, ext=(8<<11)|(1<<6)|0x38=0x4078
 */
static const uint8_t rom_lslq[] = {
    0x00, 0x00, 0x10, 0x00,
    0x00, 0x00, 0x00, 0x10,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    /* 0x10: MOVE.L #4, D0 (shift count) */
    0x20, 0x3C, 0x00, 0x00, 0x00, 0x04,
    /* 0x16: MOVE.L #1, D1 (value to shift) */
    0x22, 0x3C, 0x00, 0x00, 0x00, 0x01,
    /* 0x1C: LSLQ D0,D1,E0 */
    0xFE, 0x00, 0x40, 0x78,
    /* 0x20: BRA.S . */
    0x60, 0xFE,
};

/*
 * Test 17: VPERM — byte permutation using control register.
 *   D0=0x12345678 (src1 and src2 — same register)
 *   D1=0x00000000 (ctrl, from reset — all indices=0 → select src1 byte 0 = 0x78)
 *   VPERM dst=E0, src1=D0(ext[4:0]=0), src2=D0(ext[10:6]=0), ctrl=D1(w3[4:0]=1)
 *   E0 = 0x7878787878787878
 *
 *   3-word encoding:
 *     word1: FE 3F  (VEA=0x3F sentinel, A=B=D=0)
 *     word2: 40 00  (dst=E0=8 → 8<<11=0x4000; src2=D0→ext[10:6]=0; src1=D0→ext[4:0]=0)
 *     word3: 00 01  (ctrl=D1=register 1)
 */
static const uint8_t rom_vperm[] = {
    0x00, 0x00, 0x10, 0x00,
    0x00, 0x00, 0x00, 0x10,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    /* 0x10: MOVE.L #0x12345678, D0 */
    0x20, 0x3C, 0x12, 0x34, 0x56, 0x78,
    /* 0x16: VPERM D0,D0,E0 ctrl=D1 (3 words) */
    0xFE, 0x3F, 0x40, 0x00, 0x00, 0x01,
    /* 0x1C: BRA.S . */
    0x60, 0xFE,
};

/*
 * Test 18: LSRQ — 64-bit logical shift right.
 *   D0=4 (count=a), D1=0x10 (value=b)
 *   LSRQ D0,D1→E0: E0 = 0x10 >> 4 = 1
 *   opmode=0x39, ext=(8<<11)|(1<<6)|0x39 = 0x4079
 */
static const uint8_t rom_lsrq[] = {
    0x00, 0x00, 0x10, 0x00,
    0x00, 0x00, 0x00, 0x10,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    /* 0x10: MOVE.L #4, D0 */
    0x20, 0x3C, 0x00, 0x00, 0x00, 0x04,
    /* 0x16: MOVE.L #0x10, D1 */
    0x22, 0x3C, 0x00, 0x00, 0x00, 0x10,
    /* 0x1C: LSRQ D0,D1→E0 */
    0xFE, 0x00, 0x40, 0x79,
    /* 0x20: BRA.S . */
    0x60, 0xFE,
};

/*
 * Test 19: PCMPGTW — packed signed word greater-than (result = b > a).
 *   D0=3 (a), D1=5 (b): word 0 → 5>3 → 0xFFFF; upper words → 0>0 → 0
 *   D2 = 0x0000FFFF
 *   opmode=0x2F, ext=(2<<11)|(1<<6)|0x2F = 0x106F
 */
static const uint8_t rom_pcmpgtw[] = {
    0x00, 0x00, 0x10, 0x00,
    0x00, 0x00, 0x00, 0x10,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    /* 0x10: MOVE.L #3, D0 (a) */
    0x20, 0x3C, 0x00, 0x00, 0x00, 0x03,
    /* 0x16: MOVE.L #5, D1 (b) */
    0x22, 0x3C, 0x00, 0x00, 0x00, 0x05,
    /* 0x1C: PCMPGTW D0,D1→D2 */
    0xFE, 0x00, 0x10, 0x6F,
    /* 0x20: BRA.S . */
    0x60, 0xFE,
};

/*
 * Test 20: PCMPHIB — packed unsigned byte compare-high (result = b > a unsigned).
 *   D0=0x7F (a=127), D1=0x80 (b=128): byte 0 → 128>127 unsigned → 0xFF; others 0
 *   D2 = 0x000000FF
 *   opmode=0x22, ext=(2<<11)|(1<<6)|0x22 = 0x1062
 */
static const uint8_t rom_pcmphib[] = {
    0x00, 0x00, 0x10, 0x00,
    0x00, 0x00, 0x00, 0x10,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    /* 0x10: MOVE.L #0x7F, D0 (a) */
    0x20, 0x3C, 0x00, 0x00, 0x00, 0x7F,
    /* 0x16: MOVE.L #0x80, D1 (b) */
    0x22, 0x3C, 0x00, 0x00, 0x00, 0x80,
    /* 0x1C: PCMPHIB D0,D1→D2 */
    0xFE, 0x00, 0x10, 0x62,
    /* 0x20: BRA.S . */
    0x60, 0xFE,
};

/*
 * Test 21: PCMPHIW — packed unsigned word compare-high (result = b > a unsigned).
 *   D0=0x7FFF (a=32767), D1=0x8000 (b=32768): word 0 → 32768>32767 unsigned → 0xFFFF; others 0
 *   D2 = 0x0000FFFF
 *   opmode=0x23, ext=(2<<11)|(1<<6)|0x23 = 0x1063
 */
static const uint8_t rom_pcmphiw[] = {
    0x00, 0x00, 0x10, 0x00,
    0x00, 0x00, 0x00, 0x10,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    /* 0x10: MOVE.L #0x7FFF, D0 (a) */
    0x20, 0x3C, 0x00, 0x00, 0x7F, 0xFF,
    /* 0x16: MOVE.L #0x8000, D1 (b) */
    0x22, 0x3C, 0x00, 0x00, 0x80, 0x00,
    /* 0x1C: PCMPHIW D0,D1→D2 */
    0xFE, 0x00, 0x10, 0x63,
    /* 0x20: BRA.S . */
    0x60, 0xFE,
};

/*
 * Test 22: BFLYB — byte butterfly, writes register pair D2:D3.
 *   D0=0x02020202 (a: 4 bytes of 2), D1=0x01010101 (b: 4 bytes of 1)
 *   BFLYB D0,D1→D2:D3:
 *     D2 = sum  = {2+1, 2+1, 2+1, 2+1} = 0x03030303
 *     D3 = diff = {2-1, 2-1, 2-1, 2-1} = 0x01010101
 *   opmode=0x1C, ext=(2<<11)|(1<<6)|0x1C = 0x105C
 */
static const uint8_t rom_bflyb[] = {
    0x00, 0x00, 0x10, 0x00,
    0x00, 0x00, 0x00, 0x10,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    /* 0x10: MOVE.L #0x02020202, D0 */
    0x20, 0x3C, 0x02, 0x02, 0x02, 0x02,
    /* 0x16: MOVE.L #0x01010101, D1 */
    0x22, 0x3C, 0x01, 0x01, 0x01, 0x01,
    /* 0x1C: BFLYB D0,D1→D2:D3 */
    0xFE, 0x00, 0x10, 0x5C,
    /* 0x20: BRA.S . */
    0x60, 0xFE,
};

/*
 * Test 23: BFLYW — word butterfly, writes register pair D2:D3.
 *   D0=0x00020001 (a: words 1,2), D1=0x00010001 (b: words 1,1)
 *   BFLYW D0,D1→D2:D3:
 *     D2 = sum  = {1+1=2, 2+1=3} = 0x00030002
 *     D3 = diff = {1-1=0, 2-1=1} = 0x00010000
 *   opmode=0x1D, ext=(2<<11)|(1<<6)|0x1D = 0x105D
 */
static const uint8_t rom_bflyw[] = {
    0x00, 0x00, 0x10, 0x00,
    0x00, 0x00, 0x00, 0x10,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    /* 0x10: MOVE.L #0x00020001, D0 */
    0x20, 0x3C, 0x00, 0x02, 0x00, 0x01,
    /* 0x16: MOVE.L #0x00010001, D1 */
    0x22, 0x3C, 0x00, 0x01, 0x00, 0x01,
    /* 0x1C: BFLYW D0,D1→D2:D3 */
    0xFE, 0x00, 0x10, 0x5D,
    /* 0x20: BRA.S . */
    0x60, 0xFE,
};

/*
 * Test 24: STOREC — copy first N bytes of src to dst.
 *   D0=0x01020304 (a=src), D1=3 (b=count), E0=0 (dst, from reset)
 *   STOREC D0,D1→E0: copies bytes 0,1,2 from D0 (=0x04,0x03,0x02) to E0
 *   E0 = 0x0000000000020304
 *   opmode=0x24, ext=(8<<11)|(1<<6)|0x24 = 0x4064
 */
static const uint8_t rom_storec[] = {
    0x00, 0x00, 0x10, 0x00,
    0x00, 0x00, 0x00, 0x10,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    /* 0x10: MOVE.L #0x01020304, D0 */
    0x20, 0x3C, 0x01, 0x02, 0x03, 0x04,
    /* 0x16: MOVE.L #3, D1 */
    0x22, 0x3C, 0x00, 0x00, 0x00, 0x03,
    /* 0x1C: STOREC D0,D1→E0 */
    0xFE, 0x00, 0x40, 0x64,
    /* 0x20: BRA.S . */
    0x60, 0xFE,
};

/*
 * Test 25: STOREILM — store with inverted long mask.
 *   D0=0x01020304 (a=src), D1=0x00008000 (b=mask: byte 1 has bit7=1 → skip)
 *   E0=0 initially.
 *   byte 0: b[0]=0x00 bit7=0 → copy a[0]=0x04
 *   byte 1: b[1]=0x80 bit7=1 → keep 0x00
 *   byte 2: b[2]=0x00 bit7=0 → copy a[2]=0x02
 *   byte 3: b[3]=0x00 bit7=0 → copy a[3]=0x01
 *   E0 = 0x0000000001020004
 *   opmode=0x25, ext=(8<<11)|(1<<6)|0x25 = 0x4065
 */
static const uint8_t rom_storeilm[] = {
    0x00, 0x00, 0x10, 0x00,
    0x00, 0x00, 0x00, 0x10,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    /* 0x10: MOVE.L #0x01020304, D0 */
    0x20, 0x3C, 0x01, 0x02, 0x03, 0x04,
    /* 0x16: MOVE.L #0x00008000, D1 */
    0x22, 0x3C, 0x00, 0x00, 0x80, 0x00,
    /* 0x1C: STOREILM D0,D1→E0 */
    0xFE, 0x00, 0x40, 0x65,
    /* 0x20: BRA.S . */
    0x60, 0xFE,
};

/*
 * Test 26: UNPACK1632 — convert 4×RGB565 words to 4×ARGB32 pixels across E0:E1.
 *   D0=0x0000F800 (word0=0xF800=pure-red RGB565, word1=0x0000)
 *   UNPACK1632 D0 → E0:E1:
 *     Pixel 0 (w=0xF800): R5=0x1F→R8=0xFF, G6=0→G8=0, B5=0→B8=0
 *       alpha=0xFF, red=0xFF, green=0x00, blue=0x00
 *       pixel = 0x000000FF (as uint32 LE: byte0=alpha, byte1=red, byte2=green, byte3=blue)
 *               = 0x0000FFFF
 *     Pixel 1 (w=0x0000): alpha=0xFF, rgb=0 → pixel=0x000000FF
 *     E0 = pixel0 | (pixel1 << 32) = 0x0000FFFF | (0xFF << 32) = 0x000000FF0000FFFF
 *     Pixel 2,3 also w=0x0000 → pixel=0xFF each
 *     E1 = 0xFF | (0xFF << 32) = 0x000000FF000000FF
 *   opmode=0x1E, first=0xFE00 (VEA=D0=reg0), ext=(8<<11)|0x1E=0x401E
 */
static const uint8_t rom_unpack1632[] = {
    0x00, 0x00, 0x10, 0x00,
    0x00, 0x00, 0x00, 0x10,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    /* 0x10: MOVE.L #0x0000F800, D0 (pure-red RGB565 in word0) */
    0x20, 0x3C, 0x00, 0x00, 0xF8, 0x00,
    /* 0x16: UNPACK1632 D0 → E0:E1 */
    0xFE, 0x00, 0x40, 0x1E,
    /* 0x1A: BRA.S . */
    0x60, 0xFE,
};

/*
 * Test 27: PACK3216 — convert 2 ARGB32 pixel pairs to 4×RGB565 words.
 *   D0=0x0000F8FF (byte0=alpha=0xFF, byte1=R=0xF8, byte2=G=0x00, byte3=B=0x00)
 *     Pixel 0 (base=0): R=0xF8, G=0x00, B=0x00 → RGB565=0xF800
 *     Pixel 1 (base=4): bytes4-7=0 → RGB565=0x0000
 *   D1=0x00000000 → pixels 2,3 = 0x0000
 *   PACK3216 D0,D1→E0: E0[15:0]=0xF800, rest=0 → E0=0x000000000000F800
 *   opmode=0x07, first=0xFE00, ext=(8<<11)|(1<<6)|0x07=0x4047
 */
static const uint8_t rom_pack3216[] = {
    0x00, 0x00, 0x10, 0x00,
    0x00, 0x00, 0x00, 0x10,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    /* 0x10: MOVE.L #0x0000F8FF, D0 (A=0xFF,R=0xF8,G=0x00,B=0x00 in LE byte order) */
    0x20, 0x3C, 0x00, 0x00, 0xF8, 0xFF,
    /* 0x16: MOVE.L #0, D1 */
    0x22, 0x3C, 0x00, 0x00, 0x00, 0x00,
    /* 0x1C: PACK3216 D0,D1→E0 */
    0xFE, 0x00, 0x40, 0x47,
    /* 0x20: BRA.S . */
    0x60, 0xFE,
};

/*
 * Test 28: STOREM — masked byte blend into dst.
 *   D0=0xFFFFFFFF (a=src), D1=0x80 (b: mask low byte = 0x80 → bit7=1 → copy byte 0)
 *   E0=0 initially.
 *   Only byte 0 selected (mask bit 7 → byte index 0): E0[7:0] = a[0] = 0xFF
 *   E0 = 0x00000000000000FF
 *   opmode=0x05, ext=(8<<11)|(1<<6)|0x05 = 0x4045
 */
static const uint8_t rom_storem[] = {
    0x00, 0x00, 0x10, 0x00,
    0x00, 0x00, 0x00, 0x10,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    /* 0x10: MOVE.L #0xFFFFFFFF, D0 */
    0x20, 0x3C, 0xFF, 0xFF, 0xFF, 0xFF,
    /* 0x16: MOVE.L #0x80, D1 */
    0x22, 0x3C, 0x00, 0x00, 0x00, 0x80,
    /* 0x1C: STOREM D0,D1→E0 */
    0xFE, 0x00, 0x40, 0x45,
    /* 0x20: BRA.S . */
    0x60, 0xFE,
};


/*
 * Test 28: STOREM3 — cookie-cut store, mode 1 (byte != 0).
 *   D0 = 0x00FF003400120000 as source data in E0.
 *   Mode 1: write bytes != 0.  Destination E1 (initially 0).
 *   Bytes: 0x00,0xFF,0x00,0x34,0x00,0x12,0x00,0x00
 *   Non-zero bytes: index 1(0xFF), 3(0x34), 5(0x12)
 *
 *   For register form, STOREM3 just copies (no masking).
 *   opmode=0x26, first=0xFE08 (VEA=E0, mode=1, reg=0), ext=(9<<11)|(1<<6)|0x26=0x4866
 *
 *   Actually, we'll use a simpler approach: STOREM3 register form = copy.
 *   Load E0 with data, STOREM3 E0,#1,E1 → E1 = E0 (register form = copy).
 *   Expected: E1 = E0
 */
static const uint8_t rom_storem3[] = {
    0x00, 0x00, 0x10, 0x00,
    0x00, 0x00, 0x00, 0x10,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    /* 0x10: MOVE.L #0x12345678, D0 */
    0x20, 0x3C, 0x12, 0x34, 0x56, 0x78,
    /* 0x16: STOREM3 D0,#1,D2: register form = copy D0→D2
     * VEA = D0: mode=0, reg=0; first = 0xFE00 (A=0,B=0,D=0)
     * ext = (D2_low=2)<<11 | (mode=1)<<6 | 0x26 = 0x1066 */
    0xFE, 0x00, 0x10, 0x66,
    /* 0x1A: BRA.S . */
    0x60, 0xFE,
};

/*
 * Test 29: PMULA — alpha blend with PRM semantics.
 *   Code layout: byte 0 (LSB) = alpha, bytes 1-3 = color channels.
 *   D0 (a): alpha=0x40(64), ch1=0x10(16), ch2=0x62(98), ch3=0xDC(220) → 0xDC621040
 *   D1 (b): ch1=0xFF(255), ch2=0x80(128), ch3=0xB0(176)              → 0xB080FF00
 *   PMULA D0,D1,D2: per-pixel alpha blend.
 *   alpha=0x40=64: ch1=(64*0xFF)>>8+0x10=63+16=0x4F
 *                  ch2=(64*0x80)>>8+0x62=32+98=0x82
 *                  ch3=(64*0xB0)>>8+0xDC=44+220=264→255 (saturated)
 *   D2 = 0xFF824F00 (alpha byte=0, ch1=0x4F, ch2=0x82, ch3=0xFF)
 *
 *   opmode=0x19, ext=(2<<11)|(1<<6)|0x19=0x1059
 */
static const uint8_t rom_pmula[] = {
    0x00, 0x00, 0x10, 0x00,
    0x00, 0x00, 0x00, 0x10,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    /* 0x10: MOVE.L #$DC621040, D0 (a: alpha=0x40 in byte0) */
    0x20, 0x3C, 0xDC, 0x62, 0x10, 0x40,
    /* 0x16: MOVE.L #$B080FF00, D1 (b: ch1=0xFF, ch2=0x80, ch3=0xB0) */
    0x22, 0x3C, 0xB0, 0x80, 0xFF, 0x00,
    /* 0x1C: PMULA D0,D1,D2 */
    0xFE, 0x00, 0x10, 0x59,
    /* 0x20: BRA.S . */
    0x60, 0xFE,
};

/*
 * Test 30: MINITERM — blitter-style boolean op.
 *   4 consecutive source registers D0-D3:
 *     D0 = channel A = 0xFF00FF00
 *     D1 = channel B = 0xF0F0F0F0
 *     D2 = channel C = 0x0000FFFF
 *     D3 = miniterm  = 0x000000E2  (E2 = BSEL-like: (A&B)|(~A&~B&C))
 *   Result in E0: for each bit: mt[(a<<2)|(b<<1)|c]
 *   mt=0xE2=11100010: mt[0]=0,mt[1]=1,mt[2]=0,mt[3]=0,mt[4]=0,mt[5]=1,mt[6]=1,mt[7]=1
 *   Per-nibble (va,vb,vc → idx → mt bit):
 *     nib0: (0,0,1)→1→1=F  nib1: (0,1,1)→3→0=0  nib2: (1,0,1)→5→1=F  nib3: (1,1,1)→7→1=F
 *     nib4: (0,0,0)→0→0=0  nib5: (0,1,0)→2→0=0  nib6: (1,0,0)→4→0=0  nib7: (1,1,0)→6→1=F
 *   Result (MSB→LSB): F,0,0,0,F,F,0,F = 0xF000FF0F
 *
 *   MINITERM D0-D3,E0: VEA mode=0, reg=0 → D0 base, first=0xFE40 (A=0,B=0,D=1, mode=0, reg=0)
 *   ext=(E0=8, low4=8)<<11 | 0x2A = 0x402A
 *   Wait, D_bit is in first word bit 6. E0=reg 8, D_bit=0 means regs 0-15.
 *   dst=E0=8→ dst_low=8, D_bit=0 → ext = (8<<11)|0x2A = 0x402A
 *   first=0xFE00 (VEA=D0, mode=0, reg=0, A=B=D=0)
 */
static const uint8_t rom_miniterm[] = {
    0x00, 0x00, 0x10, 0x00,
    0x00, 0x00, 0x00, 0x10,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    /* 0x10: MOVE.L #0xFF00FF00, D0 (channel A) */
    0x20, 0x3C, 0xFF, 0x00, 0xFF, 0x00,
    /* 0x16: MOVE.L #0xF0F0F0F0, D1 (channel B) */
    0x22, 0x3C, 0xF0, 0xF0, 0xF0, 0xF0,
    /* 0x1C: MOVE.L #0x0000FFFF, D2 (channel C) */
    0x24, 0x3C, 0x00, 0x00, 0xFF, 0xFF,
    /* 0x22: MOVE.L #0xE2, D3 (miniterm byte) */
    0x26, 0x3C, 0x00, 0x00, 0x00, 0xE2,
    /* 0x28: MINITERM D0-D3,E0 */
    0xFE, 0x00, 0x40, 0x2A,
    /* 0x2C: BRA.S . */
    0x60, 0xFE,
};


/* ------------------------------------------------------------------ */
/* Test table                                                          */
/* ------------------------------------------------------------------ */

static const builtin_test_t tests[] = {
    { "lpstop_68080",  rom_lpstop_68080, sizeof(rom_lpstop_68080), "LPSTOP inherited from 68060",      3 },
    { "paddb_dreg",    rom_paddb,        sizeof(rom_paddb),        "PADDB D0,D1→D2: 0x05050505",       5 },
    { "peor_zero",     rom_peor_zero,    sizeof(rom_peor_zero),    "PEOR D0,D0→D1: self-XOR=0",        4 },
    { "paddb_ereg",    rom_paddb_ereg,   sizeof(rom_paddb_ereg),   "PADDB D0,D1→E0: E-register",       5 },
    { "psubb_order",   rom_psubb,        sizeof(rom_psubb),        "PSUBB D0,D1→D2: D=B-A=0x02020202", 4 },
    { "pmul88_shift",  rom_pmul88,       sizeof(rom_pmul88),       "PMUL88 >>8: 16*16>>8=1 per lane",   4 },
    { "load_store_ea", rom_load_store,   sizeof(rom_load_store),   "STORE E0,(A0) + LOAD (A0),E1",      7 },
    { "pmaxuw",        rom_pmaxuw,       sizeof(rom_pmaxuw),       "PMAXUW D0,D1→D2: 0x00030002",       4 },
    { "packuswb",      rom_packuswb,     sizeof(rom_packuswb),     "PACKUSWB D0,D1→E0: 8-byte packed",  4 },
    { "paddb_mem_ea",  rom_paddb_mem,    sizeof(rom_paddb_mem),    "PADDB (A0),D1→D2: mem+reg arith",   4 },
    { "transhi_xpose",  rom_transhi,      sizeof(rom_transhi),      "TRANSHI E0-E3→E4:E5: col-0,1 extract", 8 },
    { "translo_xpose",  rom_translo,      sizeof(rom_translo),      "TRANSLO E0-E3→E4:E5: col-2,3 extract", 8 },
    { "c2p_bit_xpose", rom_c2p,          sizeof(rom_c2p),          "C2P D0→E0: bit-matrix transpose",   3 },
    { "bsel_blend",    rom_bsel,         sizeof(rom_bsel),         "BSEL D0,D1→D2: ternary blend",       4 },
    { "pcmpgtb_cmp",   rom_pcmpgtb,      sizeof(rom_pcmpgtb),      "PCMPGTB D0,D1→D2: b>a 5>3 → 0xFF",  4 },
    { "lslq_shift",    rom_lslq,         sizeof(rom_lslq),         "LSLQ D0,D1→E0: 1<<4=0x10",          4 },
    { "vperm_bytes",   rom_vperm,        sizeof(rom_vperm),        "VPERM D0,D0→E0 ctrl=D1: 0x7878...", 3 },
    { "lsrq_shift",    rom_lsrq,         sizeof(rom_lsrq),         "LSRQ D0,D1→E0: 0x10>>4=1",          4 },
    { "pcmpgtw_cmp",   rom_pcmpgtw,      sizeof(rom_pcmpgtw),      "PCMPGTW D0,D1→D2: b>a 5>3 → 0xFFFF",4 },
    { "pcmphib_cmp",   rom_pcmphib,      sizeof(rom_pcmphib),      "PCMPHIB D0,D1→D2: b>a 0x80>0x7F",   4 },
    { "pcmphiw_cmp",   rom_pcmphiw,      sizeof(rom_pcmphiw),      "PCMPHIW D0,D1→D2: b>a 0x8000>0x7FFF",4 },
    { "bflyb_pair",    rom_bflyb,        sizeof(rom_bflyb),        "BFLYB D0,D1→D2:D3 sum/diff pair",    4 },
    { "bflyw_pair",    rom_bflyw,        sizeof(rom_bflyw),        "BFLYW D0,D1→D2:D3 sum/diff pair",    4 },
    { "storec_count",  rom_storec,       sizeof(rom_storec),       "STOREC D0,D1→E0: first 3 bytes",     4 },
    { "storeilm_mask", rom_storeilm,     sizeof(rom_storeilm),     "STOREILM D0,D1→E0: masked blend",    4 },
    { "unpack1632_rgb", rom_unpack1632,   sizeof(rom_unpack1632),   "UNPACK1632 D0→E0:E1: RGB565→ARGB32", 3 },
    { "pack3216_rgb",   rom_pack3216,     sizeof(rom_pack3216),     "PACK3216 D0,D1→E0: ARGB32→RGB565",   4 },
    { "storem_byte",    rom_storem,       sizeof(rom_storem),       "STOREM D0,D1→E0: mask=0x80→byte0",   4 },
    { "storem3_copy",   rom_storem3,      sizeof(rom_storem3),      "STOREM3 D0,#1,D2: reg-form copy",    4 },
    { "pmula_blend",    rom_pmula,        sizeof(rom_pmula),        "PMULA alpha-blend D0,D1→D2",         4 },
    { "miniterm_e2",    rom_miniterm,     sizeof(rom_miniterm),     "MINITERM D0-D3→E0: $E2 bsel-like",   6 },
};

#define NUM_TESTS (sizeof(tests) / sizeof(tests[0]))

/* ------------------------------------------------------------------ */
/* Pass/fail criteria                                                  */
/* ------------------------------------------------------------------ */

static int check_result(size_t idx)
{
    switch (idx) {
    case 0: return cpu.d[0] == 77 && cpu.halted == 1;
    case 1: return cpu.d[2] == 0x05050505u;
    case 2: return cpu.d[1] == 0x00000000u;
    case 3: return cpu.e[0] == 0x0000000003030303ULL;
    case 4: return cpu.d[2] == 0x02020202u;
    case 5: return cpu.d[2] == 0x00010001u;
    case 6: return cpu.e[1] == 0x0000000003030303ULL;
    case 7: return cpu.d[2] == 0x00020003u;     /* pmaxuw: lane0=max(3,2)=3, lane1=max(1,2)=2 */
    case 8: return cpu.e[0] == 0x0000040300000201ULL; /* packuswb: full 8-byte result in E0 */
    case  9: return cpu.d[2] == 0x03030303u;                        /* paddb_mem_ea  */
    case 10: return cpu.e[4] == 0x0010000C00080004ULL &&            /* transhi_xpose  */
                    cpu.e[5] == 0x000F000B00070003ULL;
    case 11: return cpu.e[4] == 0x000E000A00060002ULL &&            /* translo_xpose  */
                    cpu.e[5] == 0x000D000900050001ULL;
    case 12: return cpu.e[0] == 0x8000000000000000ULL;              /* c2p_bit_xpose  */
    case 13: return cpu.d[2] == 0xF0F0F0F0u;                        /* bsel_blend     */
    case 14: return cpu.d[2] == 0x000000FFu;                        /* pcmpgtb_cmp    */
    case 15: return cpu.e[0] == 0x0000000000000010ULL;              /* lslq_shift     */
    case 16: return cpu.e[0] == 0x7878787878787878ULL;              /* vperm_bytes    */
    case 17: return cpu.e[0] == 1ULL;                              /* lsrq_shift     */
    case 18: return cpu.d[2] == 0x0000FFFFu;                       /* pcmpgtw_cmp    */
    case 19: return cpu.d[2] == 0x000000FFu;                       /* pcmphib_cmp    */
    case 20: return cpu.d[2] == 0x0000FFFFu;                       /* pcmphiw_cmp    */
    case 21: return cpu.d[2] == 0x03030303u && cpu.d[3] == 0x01010101u; /* bflyb_pair */
    case 22: return cpu.d[2] == 0x00030002u && cpu.d[3] == 0x00010000u; /* bflyw_pair */
    case 23: return cpu.e[0] == 0x0000000000020304ULL;             /* storec_count   */
    case 24: return cpu.e[0] == 0x0000000001020004ULL;             /* storeilm_mask  */
    case 25: return cpu.e[0] == 0x000000FF0000FFFFULL &&            /* unpack1632_rgb */
                    cpu.e[1] == 0x000000FF000000FFULL;
    case 26: return cpu.e[0] == 0x000000000000F800ULL;             /* pack3216_rgb   */
    case 27: return cpu.e[0] == 0x00000000000000FFULL;             /* storem_byte    */
    case 28: return cpu.d[2] == 0x12345678u;                       /* storem3_copy   */
    case 29: return cpu.d[2] == 0xFF824F00u;                       /* pmula_blend    */
    case 30: return cpu.e[0] == 0x00000000F000FF0FULL;             /* miniterm_e2    */
    default: return 0;
    }
}

/* ------------------------------------------------------------------ */
/* Runner                                                              */
/* ------------------------------------------------------------------ */

int run_68080_tests(void)
{
    int failed = 0;

    printf("Running 68080 tests...\n");
    fflush(stdout);

    /* Switch to 68080 mode for this suite. */
    cpu_init(CPU_MODEL_68080);

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

        int pass = check_result(i);
        if (pass) {
            printf("  %-22s PASS\n", t->name);
        } else {
            printf("  %-22s FAIL  (D2=%08X D3=%08X E0=%016llX E1=%016llX E2=%016llX halted=%d)\n",
                   t->name,
                   cpu.d[2],
                   cpu.d[3],
                   (unsigned long long)cpu.e[0],
                   (unsigned long long)cpu.e[1],
                   (unsigned long long)cpu.e[2],
                   cpu.halted);
            failed = 1;
        }
    }

    /* Restore to 68000 so subsequent tests are not affected. */
    cpu_init(CPU_MODEL_68000);

    return failed;
}

const builtin_test_t *find_68080_test(const char *name)
{
    for (size_t i = 0; i < NUM_TESTS; i++) {
        if (strcmp(tests[i].name, name) == 0)
            return &tests[i];
    }
    return NULL;
}

/*
 * 68K CPU Emulator - Learning Project
 *
 * Run with: ./68k-emu [rom.bin]
 * Without a ROM, runs a tiny built-in NOP loop to verify the CPU executes.
 */

#include "cpu.h"
#include "memory.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Minimal test: reset vector at 0, code at 0x10 */
static const uint8_t nop_loop[] = {
    0x00, 0x00, 0x00, 0x10,   /* Reset: PC = 0x00000010 */
    0x00, 0x00, 0x10, 0x00,   /* Reset: SP = 0x00001000 */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  /* Padding */
    0x4E, 0x71,               /* 0x10: NOP */
    0x60, 0xFE,               /* 0x12: BRA.S -2 (branch to NOP) */
};

/* MOVE.L test: copies D0->D1, D1->D2, then loops */
static const uint8_t move_test[] = {
    0x00, 0x00, 0x00, 0x10,   /* Reset: PC = 0x00000010 */
    0x00, 0x00, 0x10, 0x00,   /* Reset: SP = 0x00001000 */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  /* Padding */
    0x20, 0x08,               /* 0x10: MOVE.L D0, D1 */
    0x20, 0x11,               /* 0x12: MOVE.L D1, D2 */
    0x4E, 0x71,               /* 0x14: NOP */
    0x60, 0xFC,               /* 0x16: BRA.S -4 (loop back to MOVE) */
};

/* MOVE.L memory test: store D0 to (A7), load into D1. Uses stack at 0x1000. */
static const uint8_t move_mem_test[] = {
    0x00, 0x00, 0x00, 0x10,   /* Reset: PC = 0x00000010 */
    0x00, 0x00, 0x10, 0x00,   /* Reset: SP = 0x00001000 */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  /* Padding */
    0x70, 0x2A,               /* 0x10: MOVEQ #42, D0 */
    0x25, 0xC0,               /* 0x12: MOVE.L D0, (A7) - store to 0x1000 */
    0x20, 0x57,               /* 0x14: MOVE.L (A7), D1 - load from 0x1000 */
    0x4E, 0x71,               /* 0x16: NOP */
    0x60, 0xFC,               /* 0x18: BRA.S -4 (loop) */
};

/* MOVE.W test: MOVEQ #42, D0; MOVE.W D0, D1; D1 lower word = 0x002A */
static const uint8_t move_w_test[] = {
    0x00, 0x00, 0x00, 0x10,   /* Reset: PC = 0x00000010 */
    0x00, 0x00, 0x10, 0x00,   /* Reset: SP = 0x00001000 */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  /* Padding */
    0x70, 0x2A,               /* 0x10: MOVEQ #42, D0 */
    0x30, 0x40,               /* 0x12: MOVE.W D0, D1 */
    0x4E, 0x71,               /* 0x14: NOP */
    0x60, 0xFC,               /* 0x16: BRA.S -4 (loop) */
};

/* MOVE.B test: MOVEQ #42, D0; MOVE.B D0, D1; D1 lower byte = 0x2A */
static const uint8_t move_b_test[] = {
    0x00, 0x00, 0x00, 0x10,   /* Reset: PC = 0x00000010 */
    0x00, 0x00, 0x10, 0x00,   /* Reset: SP = 0x00001000 */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  /* Padding */
    0x70, 0x2A,               /* 0x10: MOVEQ #42, D0 */
    0x10, 0x40,               /* 0x12: MOVE.B D0, D1 */
    0x4E, 0x71,               /* 0x14: NOP */
    0x60, 0xFC,               /* 0x16: BRA.S -4 (loop) */
};

/* MOVE.W memory test: store 0xFFFF to (A7), load into D1 */
static const uint8_t move_w_mem_test[] = {
    0x00, 0x00, 0x00, 0x10,   /* Reset: PC = 0x00000010 */
    0x00, 0x00, 0x10, 0x00,   /* Reset: SP = 0x00001000 */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  /* Padding */
    0x70, 0xFF,               /* 0x10: MOVEQ #-1, D0 */
    0x35, 0xC0,               /* 0x12: MOVE.W D0, (A7) */
    0x30, 0x57,               /* 0x14: MOVE.W (A7), D1 */
    0x4E, 0x71,               /* 0x16: NOP */
    0x60, 0xFC,               /* 0x18: BRA.S -4 (loop) */
};

/* MOVE.L #imm, Dn test: MOVE.L #0x12345678, D0 */
static const uint8_t move_imm_test[] = {
    0x00, 0x00, 0x00, 0x10,   /* Reset: PC = 0x00000010 */
    0x00, 0x00, 0x10, 0x00,   /* Reset: SP = 0x00001000 */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  /* Padding */
    0x20, 0x3C, 0x12, 0x34, 0x56, 0x78,   /* 0x10: MOVE.L #0x12345678, D0 */
    0x4E, 0x71,               /* 0x16: NOP */
    0x60, 0xFC,               /* 0x18: BRA.S -4 (loop) */
};

/* MOVE.L #imm, (An) test: store 0xDEADBEEF to (A7), load into D1 */
static const uint8_t move_imm_mem_test[] = {
    0x00, 0x00, 0x00, 0x10,   /* Reset: PC = 0x00000010 */
    0x00, 0x00, 0x10, 0x00,   /* Reset: SP = 0x00001000 */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  /* Padding */
    0x25, 0xFC, 0xDE, 0xAD, 0xBE, 0xEF,   /* 0x10: MOVE.L #0xDEADBEEF, (A7) */
    0x20, 0x57,               /* 0x16: MOVE.L (A7), D1 */
    0x4E, 0x71,               /* 0x18: NOP */
    0x60, 0xFC,               /* 0x1A: BRA.S -4 (loop) */
};

/* MOVE.L (An)+, Dn test: store #0x12345678 at (A7), then MOVE.L (A7)+, D1 */
static const uint8_t move_anp_test[] = {
    0x00, 0x00, 0x00, 0x10,   /* Reset: PC = 0x00000010 */
    0x00, 0x00, 0x10, 0x00,   /* Reset: SP = 0x00001000 */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  /* Padding */
    0x25, 0xFC, 0x12, 0x34, 0x56, 0x78,   /* 0x10: MOVE.L #0x12345678, (A7) - store at 0x1000 */
    0x20, 0x5F,               /* 0x16: MOVE.L (A7)+, D1 - load from 0x1000, A7 becomes 0x1004 */
    0x4E, 0x71,               /* 0x18: NOP */
    0x60, 0xFC,               /* 0x1A: BRA.S -4 (loop) */
};

/* MOVE.L d(An), Dn test: store D0 to 4(A7)=0x1004, load via 4(A7) */
static const uint8_t move_disp_test[] = {
    0x00, 0x00, 0x00, 0x10,   /* Reset: PC = 0x00000010 */
    0x00, 0x00, 0x10, 0x00,   /* Reset: SP = 0x00001000 */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  /* Padding */
    0x20, 0x3C, 0x12, 0x34, 0x56, 0x78,   /* 0x10: MOVE.L #0x12345678, D0 */
    0x2B, 0xC0, 0x00, 0x04,   /* 0x16: MOVE.L D0, 4(A7) - store at 0x1004 */
    0x20, 0x6F, 0x00, 0x04,   /* 0x1A: MOVE.L 4(A7), D1 - load from 0x1004 */
    0x4E, 0x71,               /* 0x1E: NOP */
    0x60, 0xFC,               /* 0x20: BRA.S -4 (loop) */
};

/* MOVE.B memory test: store 0xAB to (A7), load into D1 */
static const uint8_t move_b_mem_test[] = {
    0x00, 0x00, 0x00, 0x10,   /* Reset: PC = 0x00000010 */
    0x00, 0x00, 0x10, 0x00,   /* Reset: SP = 0x00001000 */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  /* Padding */
    0x70, 0xAB,               /* 0x10: MOVEQ #-85, D0 (0xAB) */
    0x15, 0xC0,               /* 0x12: MOVE.B D0, (A7) */
    0x10, 0x57,               /* 0x14: MOVE.B (A7), D1 */
    0x4E, 0x71,               /* 0x16: NOP */
    0x60, 0xFC,               /* 0x18: BRA.S -4 (loop) */
};

/* MOVEQ test: loads 42 into D0, -1 into D1, then loops */
static const uint8_t moveq_test[] = {
    0x00, 0x00, 0x00, 0x10,   /* Reset: PC = 0x00000010 */
    0x00, 0x00, 0x10, 0x00,   /* Reset: SP = 0x00001000 */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  /* Padding */
    0x70, 0x2A,               /* 0x10: MOVEQ #42, D0 */
    0x72, 0xFF,               /* 0x12: MOVEQ #-1, D1 */
    0x4E, 0x71,               /* 0x14: NOP */
    0x60, 0xFC,               /* 0x16: BRA.S -4 (loop) */
};

/* CMP.L test: compare D0 and D1 (both 10), Z flag should be set */
static const uint8_t cmp_test[] = {
    0x00, 0x00, 0x00, 0x10,   /* Reset: PC = 0x00000010 */
    0x00, 0x00, 0x10, 0x00,   /* Reset: SP = 0x00001000 */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  /* Padding */
    0x70, 0x0A,               /* 0x10: MOVEQ #10, D0 */
    0x72, 0x0A,               /* 0x12: MOVEQ #10, D1 */
    0xB8, 0x80,               /* 0x14: CMP.L D0, D1  (D1-D0=0, sets Z) */
    0x4E, 0x71,               /* 0x16: NOP */
    0x60, 0xFC,               /* 0x18: BRA.S -4 (loop) */
};

/* Bcc test: CMP 10 and 10, BEQ branches to set D2=1; CMP 10 and 11, BNE branches */
static const uint8_t bcc_test[] = {
    0x00, 0x00, 0x00, 0x10,   /* Reset: PC = 0x00000010 */
    0x00, 0x00, 0x10, 0x00,   /* Reset: SP = 0x00001000 */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  /* Padding */
    /* Phase 1: 10==10, BEQ should branch */
    0x70, 0x0A,               /* 0x10: MOVEQ #10, D0 */
    0x72, 0x0A,               /* 0x12: MOVEQ #10, D1 */
    0xB8, 0x80,               /* 0x14: CMP.L D0, D1 (Z set) */
    0x67, 0x02,               /* 0x16: BEQ.S +2 (skip MOVEQ #0 if equal) */
    0x74, 0x00,               /* 0x18: MOVEQ #0, D2 (not-equal path) */
    0x74, 0x01,               /* 0x1A: MOVEQ #1, D2 (equal path) */
    /* Phase 2: 10!=11, BNE should branch */
    0x72, 0x0B,               /* 0x1C: MOVEQ #11, D1 */
    0xB8, 0x80,               /* 0x1E: CMP.L D0, D1 (Z clear) */
    0x66, 0x02,               /* 0x20: BNE.S +2 (skip MOVEQ #0 if not equal) */
    0x74, 0x00,               /* 0x22: MOVEQ #0, D2 (equal path) */
    0x74, 0x02,               /* 0x24: MOVEQ #2, D2 (not-equal path) */
    0x4E, 0x71,               /* 0x26: NOP */
    0x60, 0xFC,               /* 0x28: BRA.S -4 (loop to NOP) */
};

/* Bcc comprehensive test: all 15 testable conditions. D2 = count of conditions that branched.
 * Expected: 15 (BRA,BSR,BHI,BLS,BCC,BCS,BNE,BEQ,BVC,BPL,BMI,BGE,BLT,BGT,BLE). BVS skipped. */
static const uint8_t bcc_all_test[] = {
    0x00, 0x00, 0x00, 0x10,   /* Reset: PC = 0x00000010 */
    0x00, 0x00, 0x10, 0x00,   /* Reset: SP = 0x00001000 */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  /* Padding */
    /* Phase 0: BRA (always) */
    0x74, 0x00,               /* MOVEQ #0, D2 - init D2 */
    0x76, 0x00,               /* MOVEQ #0, D3 - init D3 */
    0x60, 0x02,               /* BRA.S +2 (always) */
    0x76, 0x00,               /* MOVEQ #0, D3 (not-taken) */
    0x76, 0x01,               /* MOVEQ #1, D3 (taken) */
    0xE0, 0x83,               /* ADD.L D3, D2 */
    /* Phase 1: BSR - always branches, pushes return addr, then branches */
    0x76, 0x00, 0x61, 0x02, 0x76, 0x00, 0x76, 0x01, 0xE0, 0x83,               /* ADD.L D3, D2 */
    /* Phase 2: BHI (D1>D0 unsigned): D0=50, D1=100, CMP.L D0,D1 -> 50, C=0 Z=0 */
    0x70, 0x32,               /* MOVEQ #50, D0 */
    0x72, 0x64,               /* MOVEQ #100, D1 (0x64) */
    0xB8, 0x80,               /* CMP.L D0, D1 */
    0x76, 0x00, 0x62, 0x02, 0x76, 0x00, 0x76, 0x01, 0xE0, 0x83,               /* ADD.L D3, D2 */
    /* Phase 3: BLS (C||Z): D0=100, D1=50, CMP.L D0,D1 -> borrow C=1 */
    0x70, 0x64,               /* MOVEQ #100, D0 */
    0x72, 0x32,               /* MOVEQ #50, D1 */
    0xB8, 0x80,               /* CMP.L D0, D1 */
    0x76, 0x00, 0x63, 0x02, 0x76, 0x00, 0x76, 0x01, 0xE0, 0x83,               /* ADD.L D3, D2 */
    /* Phase 4: BCC (!C): D0=10, D1=10, CMP -> 0, C=0 */
    0x70, 0x0A, 0x72, 0x0A, 0xB8, 0x80,
    0x76, 0x00, 0x64, 0x02, 0x76, 0x00, 0x76, 0x01, 0xE0, 0x83,               /* ADD.L D3, D2 */
    /* Phase 5: BCS (C): D0=10, D1=5, CMP -> borrow C=1 */
    0x70, 0x0A, 0x72, 0x05, 0xB8, 0x80,
    0x76, 0x00, 0x65, 0x02, 0x76, 0x00, 0x76, 0x01, 0xE0, 0x83,               /* ADD.L D3, D2 */
    /* Phase 6: BNE (!Z): D0=10, D1=5, CMP -> Z=0 */
    0x70, 0x0A, 0x72, 0x05, 0xB8, 0x80,
    0x76, 0x00, 0x66, 0x02, 0x76, 0x00, 0x76, 0x01, 0xE0, 0x83,               /* ADD.L D3, D2 */
    /* Phase 7: BEQ (Z): D0=10, D1=10, CMP -> Z=1 */
    0x70, 0x0A, 0x72, 0x0A, 0xB8, 0x80,
    0x76, 0x00, 0x67, 0x02, 0x76, 0x00, 0x76, 0x01, 0xE0, 0x83,               /* ADD.L D3, D2 */
    /* Phase 8: BVC (!V): CMP 10,10 -> no overflow */
    0x70, 0x0A, 0x72, 0x0A, 0xB8, 0x80,
    0x76, 0x00, 0x68, 0x02, 0x76, 0x00, 0x76, 0x01, 0xE0, 0x83,               /* ADD.L D3, D2 */
    /* Phase 9: BVS - skip (need overflow; requires 32-bit immediate). Use skip pattern. */
    0x76, 0x00, 0x69, 0x06, 0x76, 0x00, 0x60, 0x04, 0x76, 0x01, 0xE0, 0x83,               /* ADD.L D3, D2 */
    /* Phase 10: BPL (!N): MOVEQ #42 sets N=0 */
    0x70, 0x2A,               /* MOVEQ #42, D0 */
    0x76, 0x00, 0x6A, 0x02, 0x76, 0x00, 0x76, 0x01, 0xE0, 0x83,               /* ADD.L D3, D2 */
    /* Phase 11: BMI (N): MOVEQ #-1 sets N=1 */
    0x70, 0xFF,               /* MOVEQ #-1, D0 */
    0x76, 0x00, 0x6B, 0x02, 0x76, 0x00, 0x76, 0x01, 0xE0, 0x83,               /* ADD.L D3, D2 */
    /* Phase 12: BGE (N==V): CMP 5,10 -> 10-5=5, N=0 V=0 */
    0x70, 0x05, 0x72, 0x0A, 0xB8, 0x80,
    0x76, 0x00, 0x6C, 0x02, 0x76, 0x00, 0x76, 0x01, 0xE0, 0x83,               /* ADD.L D3, D2 */
    /* Phase 13: BLT (N!=V): CMP 10,5 -> 5-10=-5, N=1 V=0 */
    0x70, 0x0A, 0x72, 0x05, 0xB8, 0x80,
    0x76, 0x00, 0x6D, 0x02, 0x76, 0x00, 0x76, 0x01, 0xE0, 0x83,               /* ADD.L D3, D2 */
    /* Phase 14: BGT (N==V && !Z): CMP 3,10 -> 7, N=0 V=0 Z=0 */
    0x70, 0x03, 0x72, 0x0A, 0xB8, 0x80,
    0x76, 0x00, 0x6E, 0x02, 0x76, 0x00, 0x76, 0x01, 0xE0, 0x83,               /* ADD.L D3, D2 */
    /* Phase 15: BLE (Z||N!=V): CMP 10,5 -> -5, N=1 V=0 */
    0x70, 0x0A, 0x72, 0x05, 0xB8, 0x80,
    0x76, 0x00, 0x6F, 0x02, 0x76, 0x00, 0x76, 0x01, 0xE0, 0x83,               /* ADD.L D3, D2 */
    /* Loop */
    0x4E, 0x71,               /* NOP */
    0x60, 0xFE,               /* BRA.S -2 */
};

/* BSR/RTS test: repeatedly call subroutine that sets D2=42, return, loop back to BSR */
static const uint8_t bsr_rts_test[] = {
    0x00, 0x00, 0x00, 0x10,   /* Reset: PC = 0x00000010 */
    0x00, 0x00, 0x10, 0x00,   /* Reset: SP = 0x00001000 */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  /* Padding */
    0x61, 0x06,               /* 0x10: BSR.S +6 (call subroutine at 0x18) */
    0x60, 0xFC,               /* 0x12: BRA.S -4 (loop back to BSR, call again) */
    0x00, 0x00, 0x00, 0x00,   /* 0x14: padding (4 bytes so MOVEQ at 0x18) */
    0x74, 0x2A,               /* 0x18: MOVEQ #42, D2 (subroutine) */
    0x4E, 0x75,               /* 0x1A: RTS */
};

/* SUB.L test: 50 - 8 = 42 in D1 */
static const uint8_t sub_test[] = {
    0x00, 0x00, 0x00, 0x10,   /* Reset: PC = 0x00000010 */
    0x00, 0x00, 0x10, 0x00,   /* Reset: SP = 0x00001000 */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  /* Padding */
    0x70, 0x08,               /* 0x10: MOVEQ #8, D0 */
    0x72, 0x32,               /* 0x12: MOVEQ #50, D1 */
    0x98, 0x80,               /* 0x14: SUB.L D0, D1  (D1 = 50 - 8 = 42) */
    0x4E, 0x71,               /* 0x16: NOP */
    0x60, 0xFC,               /* 0x18: BRA.S -4 (loop) */
};

/* ADD.L test: 10 + 32 = 42 in D1 */
static const uint8_t add_test[] = {
    0x00, 0x00, 0x00, 0x10,   /* Reset: PC = 0x00000010 */
    0x00, 0x00, 0x10, 0x00,   /* Reset: SP = 0x00001000 */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  /* Padding */
    0x70, 0x0A,               /* 0x10: MOVEQ #10, D0 */
    0x72, 0x20,               /* 0x12: MOVEQ #32, D1 */
    0xD8, 0x80,               /* 0x14: ADD.L D0, D1  (D1 = 10 + 32 = 42) */
    0x4E, 0x71,               /* 0x16: NOP */
    0x60, 0xFC,               /* 0x18: BRA.S -4 (loop) */
};

/* --- Untested MOVE variants (22 tests) --- */

/* MOVE.B #0xAB, D0 */
static const uint8_t move_b_imm_dn_test[] = {
    0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x10, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x10, 0x3C, 0x00, 0xAB,   /* MOVE.B #0xAB, D0 */
    0x4E, 0x71, 0x60, 0xFC,
};

/* MOVE.W #0x1234, D0 */
static const uint8_t move_w_imm_dn_test[] = {
    0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x10, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x30, 0x3C, 0x12, 0x34,   /* MOVE.W #0x1234, D0 */
    0x4E, 0x71, 0x60, 0xFC,
};

/* MOVE.B #0xAB, (A7); MOVE.B (A7), D1 to verify */
static const uint8_t move_b_imm_an_test[] = {
    0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x10, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x15, 0xFC, 0x00, 0xAB,   /* MOVE.B #0xAB, (A7) */
    0x10, 0x57,               /* MOVE.B (A7), D1 - verify */
    0x4E, 0x71, 0x60, 0xFC,
};

/* MOVE.W #0x1234, (A7); MOVE.W (A7), D1 to verify */
static const uint8_t move_w_imm_an_test[] = {
    0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x10, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x35, 0xFC, 0x12, 0x34,   /* MOVE.W #0x1234, (A7) */
    0x30, 0x57,               /* MOVE.W (A7), D1 */
    0x4E, 0x71, 0x60, 0xFC,
};

/* MOVE.L #0x12345678, 4(A7); MOVE.L 4(A7), D1 to verify */
static const uint8_t move_l_imm_disp_test[] = {
    0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x10, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x2B, 0xFC, 0x00, 0x04, 0x12, 0x34, 0x56, 0x78,   /* MOVE.L #0x12345678, 4(A7) */
    0x20, 0x6F, 0x00, 0x04,   /* MOVE.L 4(A7), D1 */
    0x4E, 0x71, 0x60, 0xFC,
};

/* MOVE.L -(A7), D1: store at -4(A7) first, then load via -(A7) */
static const uint8_t move_l_pdec_dn_test[] = {
    0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x10, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x2B, 0xFC, 0xFF, 0xFC, 0x12, 0x34, 0x56, 0x78,   /* MOVE.L #0x12345678, -4(A7) -> 0xFFC */
    0x20, 0x67,               /* MOVE.L -(A7), D1 - A7-=4, load from 0xFFC */
    0x4E, 0x71, 0x60, 0xFC,
};

/* MOVE.L D0, (A7)+: store 0x12345678 at (A7), A7+=4 */
static const uint8_t move_l_dn_anp_test[] = {
    0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x10, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x20, 0x3C, 0x12, 0x34, 0x56, 0x78,   /* MOVE.L #0x12345678, D0 */
    0x27, 0xC0,               /* MOVE.L D0, (A7)+ */
    0x4E, 0x71, 0x60, 0xFC,
};

/* MOVE.L D0, -(A7): A7-=4, store at new A7; verify with (A7)+ load */
static const uint8_t move_l_dn_pdec_test[] = {
    0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x10, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x20, 0x3C, 0x12, 0x34, 0x56, 0x78,   /* MOVE.L #0x12345678, D0 */
    0x29, 0xC0,               /* MOVE.L D0, -(A7) - store at 0xFFC */
    0x20, 0x5F,               /* MOVE.L (A7)+, D1 - load from 0xFFC, A7->0x1000 */
    0x4E, 0x71, 0x60, 0xFC,
};

/* MOVE.W (A7)+, D1: store 0x1234 at (A7), then (A7)+ loads, A7+=2 */
static const uint8_t move_w_anp_dn_test[] = {
    0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x10, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x35, 0xFC, 0x12, 0x34,   /* MOVE.W #0x1234, (A7) */
    0x30, 0x5F,               /* MOVE.W (A7)+, D1 */
    0x4E, 0x71, 0x60, 0xFC,
};

/* MOVE.W -(A7), D1: store 0x1234 at -4(A7) so 0xFFE has it; -(A7) loads from 0xFFE */
static const uint8_t move_w_pdec_dn_test[] = {
    0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x10, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x2B, 0xFC, 0xFF, 0xFC, 0x12, 0x34, 0x12, 0x34,   /* MOVE.L #0x12341234, -4(A7) - 0x1234 at 0xFFE */
    0x30, 0x67,               /* MOVE.W -(A7), D1 - A7-=2 to 0xFFE, load 0x1234 */
    0x4E, 0x71, 0x60, 0xFC,
};

/* MOVE.W D0, (A7)+: store at 0x1000, A7->0x1002; verify with -2(A7) */
static const uint8_t move_w_dn_anp_test[] = {
    0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x10, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x30, 0x3C, 0x12, 0x34,   /* MOVE.W #0x1234, D0 */
    0x37, 0xC0,               /* MOVE.W D0, (A7)+ - store at 0x1000 */
    0x30, 0x6F, 0xFF, 0xFE,   /* MOVE.W -2(A7), D1 - verify from 0x1000 */
    0x4E, 0x71, 0x60, 0xFC,
};

/* MOVE.W D0, -(A7): store at 0xFFE, A7=0xFFE; verify with 0(A7) */
static const uint8_t move_w_dn_pdec_test[] = {
    0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x10, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x30, 0x3C, 0x12, 0x34,   /* MOVE.W #0x1234, D0 */
    0x39, 0xC0,               /* MOVE.W D0, -(A7) - store at 0xFFE */
    0x30, 0x6F, 0x00, 0x00,   /* MOVE.W 0(A7), D1 - verify from 0xFFE */
    0x4E, 0x71, 0x60, 0xFC,
};

/* MOVE.B (A7)+, D1 */
static const uint8_t move_b_anp_dn_test[] = {
    0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x10, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x15, 0xFC, 0x00, 0xAB,   /* MOVE.B #0xAB, (A7) */
    0x10, 0x5F,               /* MOVE.B (A7)+, D1 - A7 increments by 2 (word align) */
    0x4E, 0x71, 0x60, 0xFC,
};

/* MOVE.B -(A7), D1: store 0xAB at 0xFFE via MOVE.L; -(A7) loads from 0xFFE */
static const uint8_t move_b_pdec_dn_test[] = {
    0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x10, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x2B, 0xFC, 0xFF, 0xFC, 0x12, 0x34, 0xAB, 0x78,   /* MOVE.L #0x1234AB78, -4(A7) - 0xAB at 0xFFE */
    0x10, 0x67,               /* MOVE.B -(A7), D1 - A7-=2 to 0xFFE, load 0xAB */
    0x4E, 0x71, 0x60, 0xFC,
};

/* MOVE.B D0, (A7)+: store at 0x1000, A7->0x1002; verify with -2(A7) */
static const uint8_t move_b_dn_anp_test[] = {
    0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x10, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x70, 0xAB,               /* MOVEQ #0xAB, D0 */
    0x17, 0xC0,               /* MOVE.B D0, (A7)+ - store at 0x1000 */
    0x10, 0x6F, 0xFF, 0xFE,   /* MOVE.B -2(A7), D1 - verify */
    0x4E, 0x71, 0x60, 0xFC,
};

/* MOVE.B D0, -(A7): store at 0xFFE, A7=0xFFE; verify with 0(A7) */
static const uint8_t move_b_dn_pdec_test[] = {
    0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x10, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x70, 0xAB,               /* MOVEQ #0xAB, D0 */
    0x19, 0xC0,               /* MOVE.B D0, -(A7) - store at 0xFFE */
    0x10, 0x6F, 0x00, 0x00,   /* MOVE.B 0(A7), D1 - verify */
    0x4E, 0x71, 0x60, 0xFC,
};

/* MOVE.W d(An), D1: load from 4(A7) - high word of long at 0x1004 */
static const uint8_t move_w_disp_dn_test[] = {
    0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x10, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x2B, 0xFC, 0x00, 0x04, 0x12, 0x34, 0x56, 0x78,   /* MOVE.L #0x12345678, 4(A7) */
    0x30, 0x6F, 0x00, 0x04,   /* MOVE.W 4(A7), D1 - load 0x1234 */
    0x4E, 0x71, 0x60, 0xFC,
};

/* MOVE.W D0, d(An) */
static const uint8_t move_w_dn_disp_test[] = {
    0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x10, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x30, 0x3C, 0x12, 0x34,   /* MOVE.W #0x1234, D0 */
    0x3B, 0xC0, 0x00, 0x04,   /* MOVE.W D0, 4(A7) */
    0x30, 0x6F, 0x00, 0x04,   /* MOVE.W 4(A7), D1 - verify */
    0x4E, 0x71, 0x60, 0xFC,
};

/* MOVE.B d(An), D1: store 0xAB at 4(A7) via MOVE.L */
static const uint8_t move_b_disp_dn_test[] = {
    0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x10, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x2B, 0xFC, 0x00, 0x04, 0xAB, 0x12, 0x34, 0x56,   /* MOVE.L #0xAB123456, 4(A7) - 0xAB at 0x1004 */
    0x10, 0x6F, 0x00, 0x04,   /* MOVE.B 4(A7), D1 */
    0x4E, 0x71, 0x60, 0xFC,
};

/* MOVE.B D0, d(An) */
static const uint8_t move_b_dn_disp_test[] = {
    0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x10, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x70, 0xAB,               /* MOVEQ #0xAB, D0 */
    0x1B, 0xC0, 0x00, 0x04,   /* MOVE.B D0, 4(A7) */
    0x10, 0x6F, 0x00, 0x04,   /* MOVE.B 4(A7), D1 */
    0x4E, 0x71, 0x60, 0xFC,
};

int main(int argc, char *argv[])
{
    mem_init();
    cpu_init();

    if (argc >= 2 && (strcmp(argv[1], "move") == 0 || strcmp(argv[1], "test") == 0)) {
        mem_load_rom(move_test, sizeof(move_test));
        printf("Running MOVE.L test\n");
    } else if (argc >= 2 && strcmp(argv[1], "move_mem") == 0) {
        mem_load_rom(move_mem_test, sizeof(move_mem_test));
        printf("Running MOVE.L (An)/Dn memory test\n");
    } else if (argc >= 2 && strcmp(argv[1], "move_w") == 0) {
        mem_load_rom(move_w_test, sizeof(move_w_test));
        printf("Running MOVE.W test\n");
    } else if (argc >= 2 && strcmp(argv[1], "move_b") == 0) {
        mem_load_rom(move_b_test, sizeof(move_b_test));
        printf("Running MOVE.B test\n");
    } else if (argc >= 2 && strcmp(argv[1], "move_w_mem") == 0) {
        mem_load_rom(move_w_mem_test, sizeof(move_w_mem_test));
        printf("Running MOVE.W memory test\n");
    } else if (argc >= 2 && strcmp(argv[1], "move_b_mem") == 0) {
        mem_load_rom(move_b_mem_test, sizeof(move_b_mem_test));
        printf("Running MOVE.B memory test\n");
    } else if (argc >= 2 && strcmp(argv[1], "move_imm") == 0) {
        mem_load_rom(move_imm_test, sizeof(move_imm_test));
        printf("Running MOVE.L #imm, Dn test\n");
    } else if (argc >= 2 && strcmp(argv[1], "move_imm_mem") == 0) {
        mem_load_rom(move_imm_mem_test, sizeof(move_imm_mem_test));
        printf("Running MOVE.L #imm, (An) test\n");
    } else if (argc >= 2 && strcmp(argv[1], "move_anp") == 0) {
        mem_load_rom(move_anp_test, sizeof(move_anp_test));
        printf("Running MOVE.L (An)+ test\n");
    } else if (argc >= 2 && strcmp(argv[1], "move_disp") == 0) {
        mem_load_rom(move_disp_test, sizeof(move_disp_test));
        printf("Running MOVE.L d(An) test\n");
    } else if (argc >= 2 && strcmp(argv[1], "move_b_imm_dn") == 0) {
        mem_load_rom(move_b_imm_dn_test, sizeof(move_b_imm_dn_test));
        printf("Running MOVE.B #imm, Dn test\n");
    } else if (argc >= 2 && strcmp(argv[1], "move_w_imm_dn") == 0) {
        mem_load_rom(move_w_imm_dn_test, sizeof(move_w_imm_dn_test));
        printf("Running MOVE.W #imm, Dn test\n");
    } else if (argc >= 2 && strcmp(argv[1], "move_b_imm_an") == 0) {
        mem_load_rom(move_b_imm_an_test, sizeof(move_b_imm_an_test));
        printf("Running MOVE.B #imm, (An) test\n");
    } else if (argc >= 2 && strcmp(argv[1], "move_w_imm_an") == 0) {
        mem_load_rom(move_w_imm_an_test, sizeof(move_w_imm_an_test));
        printf("Running MOVE.W #imm, (An) test\n");
    } else if (argc >= 2 && strcmp(argv[1], "move_l_imm_disp") == 0) {
        mem_load_rom(move_l_imm_disp_test, sizeof(move_l_imm_disp_test));
        printf("Running MOVE.L #imm, d(An) test\n");
    } else if (argc >= 2 && strcmp(argv[1], "move_l_pdec_dn") == 0) {
        mem_load_rom(move_l_pdec_dn_test, sizeof(move_l_pdec_dn_test));
        printf("Running MOVE.L -(An), Dn test\n");
    } else if (argc >= 2 && strcmp(argv[1], "move_l_dn_anp") == 0) {
        mem_load_rom(move_l_dn_anp_test, sizeof(move_l_dn_anp_test));
        printf("Running MOVE.L Dn, (An)+ test\n");
    } else if (argc >= 2 && strcmp(argv[1], "move_l_dn_pdec") == 0) {
        mem_load_rom(move_l_dn_pdec_test, sizeof(move_l_dn_pdec_test));
        printf("Running MOVE.L Dn, -(An) test\n");
    } else if (argc >= 2 && strcmp(argv[1], "move_w_anp_dn") == 0) {
        mem_load_rom(move_w_anp_dn_test, sizeof(move_w_anp_dn_test));
        printf("Running MOVE.W (An)+, Dn test\n");
    } else if (argc >= 2 && strcmp(argv[1], "move_w_pdec_dn") == 0) {
        mem_load_rom(move_w_pdec_dn_test, sizeof(move_w_pdec_dn_test));
        printf("Running MOVE.W -(An), Dn test\n");
    } else if (argc >= 2 && strcmp(argv[1], "move_w_dn_anp") == 0) {
        mem_load_rom(move_w_dn_anp_test, sizeof(move_w_dn_anp_test));
        printf("Running MOVE.W Dn, (An)+ test\n");
    } else if (argc >= 2 && strcmp(argv[1], "move_w_dn_pdec") == 0) {
        mem_load_rom(move_w_dn_pdec_test, sizeof(move_w_dn_pdec_test));
        printf("Running MOVE.W Dn, -(An) test\n");
    } else if (argc >= 2 && strcmp(argv[1], "move_b_anp_dn") == 0) {
        mem_load_rom(move_b_anp_dn_test, sizeof(move_b_anp_dn_test));
        printf("Running MOVE.B (An)+, Dn test\n");
    } else if (argc >= 2 && strcmp(argv[1], "move_b_pdec_dn") == 0) {
        mem_load_rom(move_b_pdec_dn_test, sizeof(move_b_pdec_dn_test));
        printf("Running MOVE.B -(An), Dn test\n");
    } else if (argc >= 2 && strcmp(argv[1], "move_b_dn_anp") == 0) {
        mem_load_rom(move_b_dn_anp_test, sizeof(move_b_dn_anp_test));
        printf("Running MOVE.B Dn, (An)+ test\n");
    } else if (argc >= 2 && strcmp(argv[1], "move_b_dn_pdec") == 0) {
        mem_load_rom(move_b_dn_pdec_test, sizeof(move_b_dn_pdec_test));
        printf("Running MOVE.B Dn, -(An) test\n");
    } else if (argc >= 2 && strcmp(argv[1], "move_w_disp_dn") == 0) {
        mem_load_rom(move_w_disp_dn_test, sizeof(move_w_disp_dn_test));
        printf("Running MOVE.W d(An), Dn test\n");
    } else if (argc >= 2 && strcmp(argv[1], "move_w_dn_disp") == 0) {
        mem_load_rom(move_w_dn_disp_test, sizeof(move_w_dn_disp_test));
        printf("Running MOVE.W Dn, d(An) test\n");
    } else if (argc >= 2 && strcmp(argv[1], "move_b_disp_dn") == 0) {
        mem_load_rom(move_b_disp_dn_test, sizeof(move_b_disp_dn_test));
        printf("Running MOVE.B d(An), Dn test\n");
    } else if (argc >= 2 && strcmp(argv[1], "move_b_dn_disp") == 0) {
        mem_load_rom(move_b_dn_disp_test, sizeof(move_b_dn_disp_test));
        printf("Running MOVE.B Dn, d(An) test\n");
    } else if (argc >= 2 && strcmp(argv[1], "moveq") == 0) {
        mem_load_rom(moveq_test, sizeof(moveq_test));
        printf("Running MOVEQ test\n");
    } else if (argc >= 2 && strcmp(argv[1], "add") == 0) {
        mem_load_rom(add_test, sizeof(add_test));
        printf("Running ADD.L test\n");
    } else if (argc >= 2 && strcmp(argv[1], "sub") == 0) {
        mem_load_rom(sub_test, sizeof(sub_test));
        printf("Running SUB.L test\n");
    } else if (argc >= 2 && strcmp(argv[1], "cmp") == 0) {
        mem_load_rom(cmp_test, sizeof(cmp_test));
        printf("Running CMP.L test\n");
    } else if (argc >= 2 && strcmp(argv[1], "bcc") == 0) {
        mem_load_rom(bcc_test, sizeof(bcc_test));
        printf("Running Bcc (BEQ/BNE) test\n");
    } else if (argc >= 2 && strcmp(argv[1], "bcc_all") == 0) {
        mem_load_rom(bcc_all_test, sizeof(bcc_all_test));
        printf("Running Bcc comprehensive test (all 15 conditions)\n");
    } else if (argc >= 2 && strcmp(argv[1], "bsr_rts") == 0) {
        mem_load_rom(bsr_rts_test, sizeof(bsr_rts_test));
        printf("Running BSR/RTS test\n");
    } else if (argc >= 2) {
        FILE *f = fopen(argv[1], "rb");
        if (!f) {
            perror(argv[1]);
            return 1;
        }
        fseek(f, 0, SEEK_END);
        long size = ftell(f);
        fseek(f, 0, SEEK_SET);
        uint8_t *rom = malloc(size);
        if (!rom) {
            fclose(f);
            fprintf(stderr, "Out of memory\n");
            return 1;
        }
        fread(rom, 1, size, f);
        fclose(f);
        mem_load_rom(rom, size);
        free(rom);
        printf("Loaded ROM: %s (%ld bytes)\n", argv[1], size);
    } else {
        mem_load_rom(nop_loop, sizeof(nop_loop));
        printf("Running built-in NOP loop (no ROM specified)\n");
    }

    cpu_reset();
    printf("PC=0x%08X  SP=0x%08X\n", cpu.pc, cpu.a[7]);

    int steps = 0;
    int max_steps = 100;
    if (argc >= 2 && strcmp(argv[1], "bcc_all") == 0)
        max_steps = 500;
    while (steps < max_steps) {
        int c = cpu_step();
        if (c == 0)
            break;
        steps++;
    }

    printf("Executed %d instructions. PC=0x%08X %s\n",
           steps, cpu.pc, cpu.halted ? "(halted)" : "");
    if (argc >= 2 && (strcmp(argv[1], "move") == 0 || strcmp(argv[1], "test") == 0))
        printf("D0=0x%08X D1=0x%08X D2=0x%08X\n", cpu.d[0], cpu.d[1], cpu.d[2]);
    if (argc >= 2 && strcmp(argv[1], "move_mem") == 0)
        printf("D0=0x%08X D1=0x%08X (expected: both 42, store/load via A7) SR=0x%04X\n",
               cpu.d[0], cpu.d[1], cpu.sr);
    if (argc >= 2 && strcmp(argv[1], "move_w") == 0)
        printf("D0=0x%08X D1=0x%08X (expected: D0=42, D1 lower word=0x002A) SR=0x%04X\n",
               cpu.d[0], cpu.d[1], cpu.sr);
    if (argc >= 2 && strcmp(argv[1], "move_b") == 0)
        printf("D0=0x%08X D1=0x%08X (expected: D0=42, D1 lower byte=0x2A) SR=0x%04X\n",
               cpu.d[0], cpu.d[1], cpu.sr);
    if (argc >= 2 && strcmp(argv[1], "move_w_mem") == 0)
        printf("D0=0x%08X D1=0x%08X (expected: D1 lower word=0xFFFF) SR=0x%04X\n",
               cpu.d[0], cpu.d[1], cpu.sr);
    if (argc >= 2 && strcmp(argv[1], "move_b_mem") == 0)
        printf("D0=0x%08X D1=0x%08X (expected: D1 lower byte=0xAB) SR=0x%04X\n",
               cpu.d[0], cpu.d[1], cpu.sr);
    if (argc >= 2 && strcmp(argv[1], "move_imm") == 0)
        printf("D0=0x%08X (expected: 0x12345678) SR=0x%04X\n",
               cpu.d[0], cpu.sr);
    if (argc >= 2 && strcmp(argv[1], "move_imm_mem") == 0)
        printf("D1=0x%08X (expected: 0xDEADBEEF) SR=0x%04X\n",
               cpu.d[1], cpu.sr);
    if (argc >= 2 && strcmp(argv[1], "move_anp") == 0)
        printf("D1=0x%08X A7=0x%08X (expected: D1=0x12345678, A7=0x1004) SR=0x%04X\n",
               cpu.d[1], cpu.a[7], cpu.sr);
    if (argc >= 2 && strcmp(argv[1], "move_disp") == 0)
        printf("D1=0x%08X (expected: 0x12345678) SR=0x%04X\n",
               cpu.d[1], cpu.sr);
    if (argc >= 2 && strcmp(argv[1], "moveq") == 0)
        printf("D0=0x%08X D1=0x%08X (expected: D0=42, D1=0xFFFFFFFF) SR=0x%04X\n",
               cpu.d[0], cpu.d[1], cpu.sr);
    if (argc >= 2 && strcmp(argv[1], "add") == 0)
        printf("D0=0x%08X D1=0x%08X (expected: D0=10, D1=42) SR=0x%04X\n",
               cpu.d[0], cpu.d[1], cpu.sr);
    if (argc >= 2 && strcmp(argv[1], "sub") == 0)
        printf("D0=0x%08X D1=0x%08X (expected: D0=8, D1=42) SR=0x%04X\n",
               cpu.d[0], cpu.d[1], cpu.sr);
    if (argc >= 2 && strcmp(argv[1], "cmp") == 0)
        printf("D0=0x%08X D1=0x%08X (expected: both 10, Z flag set) SR=0x%04X\n",
               cpu.d[0], cpu.d[1], cpu.sr);
    if (argc >= 2 && strcmp(argv[1], "bcc") == 0)
        printf("D0=0x%08X D1=0x%08X D2=0x%08X (expected: D2=2, BEQ+BNE both branched) SR=0x%04X\n",
               cpu.d[0], cpu.d[1], cpu.d[2], cpu.sr);
    if (argc >= 2 && strcmp(argv[1], "bcc_all") == 0)
        printf("D2=0x%08X (expected: 15 conditions branched) SR=0x%04X\n",
               cpu.d[2], cpu.sr);
    if (argc >= 2 && strcmp(argv[1], "bsr_rts") == 0)
        printf("D2=0x%08X SP=0x%08X (expected: D2=42, BSR/RTS worked) SR=0x%04X\n",
               cpu.d[2], cpu.a[7], cpu.sr);
    if (argc >= 2 && strcmp(argv[1], "move_b_imm_dn") == 0)
        printf("D0=0x%08X (expected: low byte 0xAB) SR=0x%04X\n", cpu.d[0], cpu.sr);
    if (argc >= 2 && strcmp(argv[1], "move_w_imm_dn") == 0)
        printf("D0=0x%08X (expected: low word 0x1234) SR=0x%04X\n", cpu.d[0], cpu.sr);
    if (argc >= 2 && strcmp(argv[1], "move_b_imm_an") == 0)
        printf("D1=0x%08X (expected: low byte 0xAB) SR=0x%04X\n", cpu.d[1], cpu.sr);
    if (argc >= 2 && strcmp(argv[1], "move_w_imm_an") == 0)
        printf("D1=0x%08X (expected: low word 0x1234) SR=0x%04X\n", cpu.d[1], cpu.sr);
    if (argc >= 2 && strcmp(argv[1], "move_l_imm_disp") == 0)
        printf("D1=0x%08X (expected: 0x12345678) SR=0x%04X\n", cpu.d[1], cpu.sr);
    if (argc >= 2 && strcmp(argv[1], "move_l_pdec_dn") == 0)
        printf("D1=0x%08X A7=0x%08X (expected: D1=0x12345678, A7=0xFFC) SR=0x%04X\n",
               cpu.d[1], cpu.a[7], cpu.sr);
    if (argc >= 2 && strcmp(argv[1], "move_l_dn_anp") == 0)
        printf("A7=0x%08X (expected: 0x1004) SR=0x%04X\n", cpu.a[7], cpu.sr);
    if (argc >= 2 && strcmp(argv[1], "move_l_dn_pdec") == 0)
        printf("D1=0x%08X A7=0x%08X (expected: D1=0x12345678, A7=0x1000) SR=0x%04X\n",
               cpu.d[1], cpu.a[7], cpu.sr);
    if (argc >= 2 && strcmp(argv[1], "move_w_anp_dn") == 0)
        printf("D1=0x%08X A7=0x%08X (expected: D1 low word 0x1234, A7=0x1002) SR=0x%04X\n",
               cpu.d[1], cpu.a[7], cpu.sr);
    if (argc >= 2 && strcmp(argv[1], "move_w_pdec_dn") == 0)
        printf("D1=0x%08X A7=0x%08X (expected: D1 low word 0x1234, A7=0xFFE) SR=0x%04X\n",
               cpu.d[1], cpu.a[7], cpu.sr);
    if (argc >= 2 && strcmp(argv[1], "move_w_dn_anp") == 0)
        printf("D1=0x%08X (expected: low word 0x1234) SR=0x%04X\n", cpu.d[1], cpu.sr);
    if (argc >= 2 && strcmp(argv[1], "move_w_dn_pdec") == 0)
        printf("D1=0x%08X (expected: low word 0x1234) SR=0x%04X\n", cpu.d[1], cpu.sr);
    if (argc >= 2 && strcmp(argv[1], "move_b_anp_dn") == 0)
        printf("D1=0x%08X A7=0x%08X (expected: D1 low byte 0xAB, A7=0x1002) SR=0x%04X\n",
               cpu.d[1], cpu.a[7], cpu.sr);
    if (argc >= 2 && strcmp(argv[1], "move_b_pdec_dn") == 0)
        printf("D1=0x%08X A7=0x%08X (expected: D1 low byte 0xAB, A7=0xFFE) SR=0x%04X\n",
               cpu.d[1], cpu.a[7], cpu.sr);
    if (argc >= 2 && strcmp(argv[1], "move_b_dn_anp") == 0)
        printf("D1=0x%08X (expected: low byte 0xAB) SR=0x%04X\n", cpu.d[1], cpu.sr);
    if (argc >= 2 && strcmp(argv[1], "move_b_dn_pdec") == 0)
        printf("D1=0x%08X (expected: low byte 0xAB) SR=0x%04X\n", cpu.d[1], cpu.sr);
    if (argc >= 2 && strcmp(argv[1], "move_w_disp_dn") == 0)
        printf("D1=0x%08X (expected: low word 0x1234) SR=0x%04X\n", cpu.d[1], cpu.sr);
    if (argc >= 2 && strcmp(argv[1], "move_w_dn_disp") == 0)
        printf("D1=0x%08X (expected: low word 0x1234) SR=0x%04X\n", cpu.d[1], cpu.sr);
    if (argc >= 2 && strcmp(argv[1], "move_b_disp_dn") == 0)
        printf("D1=0x%08X (expected: low byte 0xAB) SR=0x%04X\n", cpu.d[1], cpu.sr);
    if (argc >= 2 && strcmp(argv[1], "move_b_dn_disp") == 0)
        printf("D1=0x%08X (expected: low byte 0xAB) SR=0x%04X\n", cpu.d[1], cpu.sr);

    return 0;
}

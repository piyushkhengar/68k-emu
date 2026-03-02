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

int main(int argc, char *argv[])
{
    mem_init();
    cpu_init();

    if (argc >= 2 && (strcmp(argv[1], "move") == 0 || strcmp(argv[1], "test") == 0)) {
        mem_load_rom(move_test, sizeof(move_test));
        printf("Running MOVE.L test\n");
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

    return 0;
}

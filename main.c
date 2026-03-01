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
    const int max_steps = 100;
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

    return 0;
}

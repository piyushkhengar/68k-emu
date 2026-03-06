/*
 * 68K CPU Emulator - Learning Project
 *
 * Run with: ./68k-emu [rom.bin]
 * Without a ROM, runs a tiny built-in NOP loop to verify the CPU executes.
 */

#include "cpu.h"
#include "memory.h"
#include "tests.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void print_cpu_state(void)
{
    printf("D0=0x%08X D1=0x%08X D2=0x%08X A7=0x%08X SR=0x%04X\n",
           cpu.d[0], cpu.d[1], cpu.d[2], cpu.a[7], cpu.sr);
}

int main(int argc, char *argv[])
{
    mem_init();
    cpu_init();

    const builtin_test_t *test = NULL;
    if (argc >= 2)
        test = find_builtin_test(argv[1]);

    if (test) {
        mem_load_rom(test->rom, test->size);
        printf("%s\n", test->description);
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
        mem_load_rom(nop_loop, nop_loop_size);
        printf("Running built-in NOP loop (no ROM specified)\n");
    }

    cpu_reset();
    printf("PC=0x%08X  SP=0x%08X\n", cpu.pc, cpu.a[7]);

    int steps = 0;
    int max_steps = test && test->max_steps ? test->max_steps : 100;
    while (steps < max_steps) {
        int c = cpu_step();
        if (c == 0)
            break;
        steps++;
    }

    printf("Executed %d instructions. PC=0x%08X %s\n",
           steps, cpu.pc, cpu.halted ? "(halted)" : "");
    if (test)
        print_cpu_state();

    return 0;
}

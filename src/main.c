/*
 * 68K CPU Emulator - Learning Project
 *
 * Run with: ./68k-emu [rom.bin|test_name] [--speed MHz]
 * Without a ROM, runs a tiny built-in NOP loop.
 * --speed 0 or omitted: hyperspeed (no throttling)
 * --speed 7.09: PAL Amiga speed (7.09 MHz)
 */

#include "cpu.h"
#include "memory.h"
#include "processor_tests.h"
#include "tests.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#endif

#define FRAME_RATE_HZ 50   /* PAL Amiga; use 60 for NTSC */

static double get_monotonic_sec(void)
{
#ifdef _WIN32
    return (double)GetTickCount64() / 1000.0;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
#endif
}

static void print_cpu_state(void)
{
    printf("D0=0x%08X D1=0x%08X D2=0x%08X A7=0x%08X SR=0x%04X\n",
           cpu.d[0], cpu.d[1], cpu.d[2], cpu.a[7], cpu.sr);
}

/* Parse argv; returns speed_mhz (0 = unlimited), sets *rom_or_test, *run_all, *processor_tests, *processor_tests_filter. */
static double parse_args(int argc, char *argv[], const char **rom_or_test, int *run_all,
                         const char **processor_tests, const char **processor_tests_filter)
{
    double speed_mhz = 0;
    *rom_or_test = NULL;
    *run_all = 0;
    *processor_tests = NULL;
    *processor_tests_filter = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--speed") == 0) {
            if (i + 1 < argc) {
                speed_mhz = strtod(argv[i + 1], NULL);
                if (speed_mhz < 0)
                    speed_mhz = 0;
                i++;
            }
        } else if (strcmp(argv[i], "--run-all-tests") == 0) {
            *run_all = 1;
        } else if (strcmp(argv[i], "--processor-tests") == 0) {
            if (i + 1 < argc) {
                *processor_tests = argv[i + 1];
                i++;
                if (i + 1 < argc && argv[i + 1][0] != '-') {
                    *processor_tests_filter = argv[i + 1];
                    i++;
                }
            }
        } else if (*rom_or_test == NULL) {
            *rom_or_test = argv[i];
        }
    }
    return speed_mhz;
}

static void sleep_sec(double sec)
{
    if (sec <= 0)
        return;
#ifdef _WIN32
    {
        DWORD ms = (DWORD)(sec * 1000.0);
        if (ms > 0)
            Sleep(ms);
    }
#else
    struct timespec req = {
        .tv_sec = (time_t)sec,
        .tv_nsec = (long)((sec - (time_t)sec) * 1e9)
    };
    if (req.tv_nsec < 0)
        req.tv_nsec = 0;
    if (req.tv_nsec >= 1000000000)
        req.tv_nsec = 999999999;
    nanosleep(&req, NULL);
#endif
}

int main(int argc, char *argv[])
{
    mem_init();
    cpu_init();

    const char *rom_or_test = NULL;
    int run_all = 0;
    const char *processor_tests = NULL;
    const char *processor_tests_filter = NULL;
    double speed_mhz = parse_args(argc, argv, &rom_or_test, &run_all, &processor_tests, &processor_tests_filter);

    if (processor_tests) {
        return run_processor_tests(processor_tests, processor_tests_filter);
    }
    if (run_all) {
        return run_all_tests(speed_mhz);
    }

    const builtin_test_t *test = NULL;
    if (rom_or_test)
        test = find_builtin_test(rom_or_test);

    if (test) {
        mem_load_rom(test->rom, test->size);
        printf("%s\n", test->description);
    } else if (rom_or_test) {
        FILE *f = fopen(rom_or_test, "rb");
        if (!f) {
            perror(rom_or_test);
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
        printf("Loaded ROM: %s (%ld bytes)\n", rom_or_test, size);
    } else {
        mem_load_rom(nop_loop, nop_loop_size);
        printf("Running built-in NOP loop (no ROM specified)\n");
    }

    cpu_reset();
    printf("PC=0x%08X  SP=0x%08X\n", cpu.pc, cpu.a[7]);

    if (speed_mhz > 0)
        printf("Running at %.2f MHz\n", speed_mhz);

    int steps = 0;
    int max_steps;
    if (test) {
        max_steps = test->max_steps ? test->max_steps : 100;
    } else if (rom_or_test) {
        max_steps = 10000000;  /* ROM file: allow long execution */
    } else {
        max_steps = 100;       /* nop_loop default */
    }
    uint64_t cycles_this_frame = 0;
    double frame_start = get_monotonic_sec();

    if (speed_mhz > 0) {
        uint64_t cycles_per_frame = (uint64_t)(speed_mhz * 1e6 / FRAME_RATE_HZ);
        double frame_sec = 1.0 / FRAME_RATE_HZ;

        while (steps < max_steps) {
            int c = cpu_step();
            if (c == 0)
                break;
            cpu.cycles += c;
            cycles_this_frame += c;
            steps++;

            if (cycles_this_frame >= cycles_per_frame) {
                double target_elapsed = frame_sec;
                double actual_elapsed = get_monotonic_sec() - frame_start;
                sleep_sec(target_elapsed - actual_elapsed);
                cycles_this_frame = 0;
                frame_start = get_monotonic_sec();
            }
        }
    } else {
        while (steps < max_steps) {
            int c = cpu_step();
            if (c == 0)
                break;
            cpu.cycles += c;
            steps++;
        }
    }

    printf("Executed %d instructions. PC=0x%08X %s\n",
           steps, cpu.pc, cpu.halted ? "(halted)" : "");
    if (test) {
        print_cpu_state();
        printf("Cycles: %u\n", (unsigned)cpu.cycles);
    }

    return 0;
}

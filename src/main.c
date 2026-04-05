/*
 * 68K CPU Emulator - Learning Project
 *
 * Run with: ./68k-emu [rom.bin|test_name] [--speed MHz]
 * Without a ROM, runs a tiny built-in NOP loop.
 * --speed 0 or omitted: hyperspeed (no throttling)
 * --speed 7.09: PAL Amiga speed (7.09 MHz)
 *
 * System emulation:
 *   ./68k-emu --system genesis rom.bin
 *   ./68k-emu --system genesis --headless rom.bin
 *
 * CPU model selection (default: 68000):
 *   ./68k-emu --cpu 68000 --system genesis rom.bin
 *   ./68k-emu --cpu 68010 --system genesis rom.bin
 *   (68010/020/030/040/060 not yet implemented — flag accepted for future use)
 */

#include "cpu.h"
#include "cpu_internal.h"
#include "memory.h"
#include "processor_tests.h"
#include "test_runner.h"
#include "system.h"
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

static cpu_model_t parse_cpu_model(const char *name)
{
    if (strcmp(name, "68000") == 0) return CPU_MODEL_68000;
    if (strcmp(name, "68010") == 0) return CPU_MODEL_68010;
    if (strcmp(name, "68020") == 0) return CPU_MODEL_68020;
    if (strcmp(name, "68030") == 0) return CPU_MODEL_68030;
    if (strcmp(name, "68040") == 0) return CPU_MODEL_68040;
    if (strcmp(name, "68060") == 0) return CPU_MODEL_68060;
    if (strcmp(name, "68080") == 0) return CPU_MODEL_68080;
    fprintf(stderr, "Unknown CPU model '%s'; defaulting to 68000\n", name);
    return CPU_MODEL_68000;
}

/* Parse argv; returns speed_mhz (0 = unlimited). */
static double parse_args(int argc, char *argv[], const char **rom_or_test, int *run_all,
                         const char **processor_tests, const char **processor_tests_filter,
                         int *max_steps_out, int *debug,
                         const char **system_name, int *headless,
                         cpu_model_t *cpu_model)
{
    double speed_mhz = 0;
    *rom_or_test = NULL;
    *run_all = 0;
    *processor_tests = NULL;
    *processor_tests_filter = NULL;
    *max_steps_out = -1;
    *debug = 0;
    *system_name = NULL;
    *headless = 0;
    *cpu_model = CPU_MODEL_68000;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--cpu") == 0) {
            if (i + 1 < argc)
                *cpu_model = parse_cpu_model(argv[++i]);
        } else if (strcmp(argv[i], "--system") == 0) {
            if (i + 1 < argc) {
                *system_name = argv[++i];
            }
        } else if (strcmp(argv[i], "--headless") == 0) {
            *headless = 1;
        } else if (strcmp(argv[i], "--debug") == 0) {
            *debug = 1;
        } else if (strcmp(argv[i], "--speed") == 0) {
            if (i + 1 < argc) {
                speed_mhz = strtod(argv[i + 1], NULL);
                if (speed_mhz < 0)
                    speed_mhz = 0;
                i++;
            }
        } else if (strcmp(argv[i], "--max-steps") == 0) {
            if (i + 1 < argc) {
                *max_steps_out = (int)strtol(argv[i + 1], NULL, 0);
                if (*max_steps_out < 0)
                    *max_steps_out = -1;
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

static void trace_jsr_cb(uint32_t addr)
{
    (void)addr;
    printf("JSR 0x%08X\n", addr);
}

static int btst_fail_reported;
static void trace_branch_to_cb(uint32_t from_pc, uint32_t to_pc)
{
    if (to_pc == 0xA38 && !btst_fail_reported) {
        btst_fail_reported = 1;
        printf("Branch to BTST_FAIL from 0x%08X\n", from_pc);
    }
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

    const char *rom_or_test = NULL;
    int run_all = 0;
    const char *processor_tests = NULL;
    const char *processor_tests_filter = NULL;
    int max_steps_arg = -1;
    int debug = 0;
    const char *system_name = NULL;
    int headless = 0;
    cpu_model_t cpu_model = CPU_MODEL_68000;
    double speed_mhz = parse_args(argc, argv, &rom_or_test, &run_all,
                                  &processor_tests, &processor_tests_filter,
                                  &max_steps_arg, &debug,
                                  &system_name, &headless,
                                  &cpu_model);

    cpu_init(cpu_model);

    if (processor_tests) {
        return run_processor_tests(processor_tests, processor_tests_filter);
    }
    if (run_all) {
        return run_all_tests(speed_mhz);
    }

    const builtin_test_t *test = NULL;
    if (rom_or_test && !system_name)
        test = find_test_by_name(rom_or_test);

    const system_t *sys = NULL;
    if (system_name) {
        sys = system_find(system_name);
        if (!sys) {
            fprintf(stderr, "Unknown system: %s\n", system_name);
            return 1;
        }
    }

    if (sys && rom_or_test) {
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

        if (sys->init(rom, (size_t)size) < 0) {
            free(rom);
            return 1;
        }
        free(rom);

        printf("%s: loaded %s (%ld bytes)\n", sys->description, rom_or_test, size);
    } else if (sys) {
        fprintf(stderr, "Usage: %s --system %s <rom.bin>\n", argv[0], sys->name);
        return 1;
    } else if (test) {
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

    if (sys) {
        if (headless)
            sys->run_headless(max_steps_arg > 0 ? max_steps_arg : 120);
        else
            sys->run();
        sys->shutdown();
        return 0;
    }

    if (debug && rom_or_test && !test) {
        btst_fail_reported = 0;
        cpu_set_trace_jsr(trace_jsr_cb);
        cpu_set_trace_branch_to(trace_branch_to_cb);
    }

    printf("PC=0x%08X  SP=0x%08X\n", cpu.pc, cpu.a[7]);

    if (speed_mhz > 0)
        printf("Running at %.2f MHz\n", speed_mhz);

    int steps = 0;
    int max_steps;
    if (max_steps_arg >= 0) {
        max_steps = max_steps_arg;
    } else if (test) {
        max_steps = test->max_steps ? test->max_steps : 100;
    } else if (rom_or_test) {
        max_steps = 500000000;
    } else {
        max_steps = 100;
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
            if (rom_or_test && cpu.pc == 0xF000)
                break;

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
            if (rom_or_test && cpu.pc == 0xF000)
                break;
        }
    }

    printf("Executed %d instructions. PC=0x%08X %s\n",
           steps, cpu.pc, cpu.halted ? "(halted)" : "");
    if (rom_or_test && !test) {
        if (cpu.pc == 0xF000)
            printf("MCL68: ALL TESTS PASSED\n");
        else
            printf("MCL68: FAILED (PC stuck at 0x%08X = *_FAIL loop)\n", cpu.pc);
    } else if (test) {
        print_cpu_state();
        printf("Cycles: %u\n", (unsigned)cpu.cycles);
    }

    return 0;
}

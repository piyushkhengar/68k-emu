/*
 * Genesis hardware tests: VDP and interrupt tests that run through the
 * Genesis bus rather than flat RAM.  Extracted from tests.c to keep
 * Genesis-specific code isolated.
 */

#include "genesis_tests.h"
#include "bus.h"
#include "vdp.h"
#include "../core/cpu.h"
#include "../core/memory.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#endif

/* ------------------------------------------------------------------ */
/*  Test ROM data                                                      */
/* ------------------------------------------------------------------ */

static const uint8_t genesis_vdp_test[0x264] = {
    0x00, 0xFF, 0x00, 0x00,
    0x00, 0x00, 0x02, 0x00,
    [0x200] =
    0x30, 0x39, 0x00, 0xC0, 0x00, 0x04,
    0x33, 0xC0, 0x00, 0xE0, 0x00, 0x00,
    0x33, 0xFC, 0x80, 0x04, 0x00, 0xC0, 0x00, 0x04,
    0x33, 0xFC, 0x81, 0x74, 0x00, 0xC0, 0x00, 0x04,
    0x33, 0xFC, 0x8F, 0x02, 0x00, 0xC0, 0x00, 0x04,
    0x33, 0xFC, 0x40, 0x00, 0x00, 0xC0, 0x00, 0x04,
    0x33, 0xFC, 0x00, 0x00, 0x00, 0xC0, 0x00, 0x04,
    0x33, 0xFC, 0xDE, 0xAD, 0x00, 0xC0, 0x00, 0x00,
    0x33, 0xFC, 0xBE, 0xEF, 0x00, 0xC0, 0x00, 0x00,
    0x33, 0xFC, 0xC0, 0x00, 0x00, 0xC0, 0x00, 0x04,
    0x33, 0xFC, 0x00, 0x00, 0x00, 0xC0, 0x00, 0x04,
    0x33, 0xFC, 0x0E, 0x00, 0x00, 0xC0, 0x00, 0x00,
    0x32, 0x00,
    0x4E, 0x72, 0x27, 0x00,
};

static const uint8_t genesis_irq_test[0x220] = {
    0x00, 0xFF, 0x00, 0x00,
    0x00, 0x00, 0x02, 0x00,
    [0x078] =
    0x00, 0x00, 0x01, 0x00,

    [0x100] =
    0x20, 0x3C, 0x12, 0x34, 0x56, 0x78,
    0x32, 0x39, 0x00, 0xC0, 0x00, 0x04,
    0x4E, 0x73,

    [0x200] =
    0x33, 0xFC, 0x81, 0x74, 0x00, 0xC0, 0x00, 0x04,
    0x46, 0xFC, 0x20, 0x00,
    0x4A, 0x80,
    0x67, 0xFC,
    0x4E, 0x72, 0x27, 0x00,
};

/* ------------------------------------------------------------------ */
/*  Test table                                                         */
/* ------------------------------------------------------------------ */

typedef struct {
    const char *name;
    const uint8_t *rom;
    size_t size;
    const char *description;
    int max_steps;
    unsigned expected_cycles;
} genesis_test_t;

static const genesis_test_t genesis_tests[] = {
    { "genesis_vdp", genesis_vdp_test, sizeof(genesis_vdp_test),
      "Running Genesis VDP test", 20, 240 },
    { "genesis_irq", genesis_irq_test, sizeof(genesis_irq_test),
      "Running Genesis interrupt test", 500, 0 },
};

#define NUM_GENESIS_TESTS (sizeof(genesis_tests) / sizeof(genesis_tests[0]))

/* ------------------------------------------------------------------ */
/*  Result checking                                                    */
/* ------------------------------------------------------------------ */

static int check_genesis_result(size_t idx)
{
    switch (idx) {
    case 0:
        return cpu.d[0] == 0x0208 && cpu.d[1] == 0x0208 &&
               cpu.halted &&
               vdp.vram[0] == 0xDE && vdp.vram[1] == 0xAD &&
               vdp.vram[2] == 0xBE && vdp.vram[3] == 0xEF &&
               vdp.cram[0] == 0x0E00 &&
               vdp.regs[0] == 0x04 && vdp.regs[1] == 0x74 &&
               vdp.regs[15] == 0x02;
    case 1:
        return cpu.d[0] == 0x12345678 && cpu.halted;
    default:
        return 0;
    }
}

/* ------------------------------------------------------------------ */
/*  Helpers                                                            */
/* ------------------------------------------------------------------ */

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

/* ------------------------------------------------------------------ */
/*  Public runner                                                      */
/* ------------------------------------------------------------------ */

#define SLICE_MS_THROTTLED 1

int run_genesis_tests(double speed_mhz)
{
    int failed = 0;

    static const mem_bus_t gbus = {
        .read8   = bus_read8,
        .read16  = bus_read16,
        .read32  = bus_read32,
        .write8  = bus_write8,
        .write16 = bus_write16,
        .write32 = bus_write32,
    };

    uint64_t cycles_this_slice = 0;
    double slice_start = get_monotonic_sec();

    for (size_t i = 0; i < NUM_GENESIS_TESTS; i++) {
        const genesis_test_t *t = &genesis_tests[i];

        bus_init(t->rom, t->size);
        mem_set_bus(&gbus);

        if (strstr(t->name, "irq"))
            vdp.line = 220;

        cpu_reset();

        int max_steps = t->max_steps ? t->max_steps : 100;
        int steps = 0;
        int scanline_acc = 0;

        while (steps < max_steps) {
            int c = cpu_step();
            if (c == 0)
                break;
            cpu.cycles += c;
            cycles_this_slice += c;
            steps++;

            scanline_acc += c;
            while (scanline_acc >= 488) {
                scanline_acc -= 488;
                vdp_run_scanline(vdp.line);
                vdp.line = (vdp.line + 1) % 262;
            }

            if (speed_mhz > 0) {
                uint64_t target_cycles = (uint64_t)(speed_mhz * 1e6 * SLICE_MS_THROTTLED / 1000);
                if (cycles_this_slice >= target_cycles) {
                    double target_elapsed = (double)cycles_this_slice / (speed_mhz * 1e6);
                    double actual_elapsed = get_monotonic_sec() - slice_start;
                    sleep_sec(target_elapsed - actual_elapsed);
                    cycles_this_slice = 0;
                    slice_start = get_monotonic_sec();
                }
            }
        }

        int pass = check_genesis_result(i);
        int cycle_ok = 1;
        if (t->expected_cycles && cpu.cycles != t->expected_cycles)
            cycle_ok = 0;

        if (pass && cycle_ok) {
            printf("  %-12s PASS\n", t->name);
        } else if (!pass) {
            printf("  %-12s FAIL\n", t->name);
            failed++;
        } else {
            printf("  %-12s FAIL (cycles: expected %u, got %u)\n",
                   t->name, t->expected_cycles, (unsigned)cpu.cycles);
            failed++;
        }

        mem_set_bus(NULL);
    }

    return failed;
}

#include "test_runner.h"
#include "tests_68000.h"
#include "tests_68010.h"
#include "tests_68020.h"
#include "tests_68030.h"
#include "tests_68040.h"
#include "tests_68060.h"
#include "tests_68080.h"
#include "machine/runner.h"
#include "memory.h"
#include <stdio.h>

/* Declared in timing_tests.c */
int run_timing_tests(void);

int run_suite_in_mode(const builtin_test_t *table, size_t count,
                      int (*check_fn)(size_t idx),
                      cpu_model_t model, const char *label)
{
    int failed = 0;
    int skipped = 0;

    printf("  [inherited] %s...\n", label);
    fflush(stdout);

    cpu_init(model);

    for (size_t i = 0; i < count; i++) {
        const builtin_test_t *t = &table[i];

        if (t->retired_after != 0 && (int)model > t->retired_after) {
            printf("    %-22s SKIP\n", t->name);
            skipped++;
            continue;
        }

        mem_set_bus(NULL);
        mem_reset();
        mem_load_rom(t->rom, t->size);
        cpu_reset();

        int max_steps = t->max_steps ? t->max_steps : 100;
        int steps = 0;
        while (steps < max_steps) {
            int c = cpu_step();
            if (c == 0)
                break;
            steps++;
        }

        int pass = check_fn(i);
        if (pass) {
            printf("    %-22s PASS\n", t->name);
        } else {
            printf("    %-22s FAIL\n", t->name);
            failed = 1;
        }
    }

    if (skipped)
        printf("    (%d test(s) skipped — retired before this model)\n", skipped);

    return failed;
}

const builtin_test_t *find_test_by_name(const char *name)
{
    const builtin_test_t *t;
    if ((t = find_68000_test(name))) return t;
    if ((t = find_68010_test(name))) return t;
    if ((t = find_68020_test(name))) return t;
    if ((t = find_68030_test(name))) return t;
    if ((t = find_68040_test(name))) return t;
    if ((t = find_68060_test(name))) return t;
    if ((t = find_68080_test(name))) return t;
    return NULL;
}

int run_all_tests(double speed_mhz)
{
    int failed = 0;

    if (run_68000_tests(speed_mhz)) failed = 1;
    if (run_68010_tests())          failed = 1;
    if (run_68020_tests())          failed = 1;
    if (run_68030_tests())          failed = 1;
    if (run_68040_tests())          failed = 1;
    if (run_68060_tests())          failed = 1;
    if (run_68080_tests())          failed = 1;
    if (run_timing_tests())         failed = 1;
    if (run_all_machine_tests(speed_mhz)) failed = 1;

    if (failed)
        printf("Some tests failed.\n");
    else
        printf("All tests passed.\n");
    return failed ? 1 : 0;
}

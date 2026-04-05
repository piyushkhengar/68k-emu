#include "test_runner.h"
#include "tests_68000.h"
#include "tests_68010.h"
#include "tests_68020.h"
#include "tests_68030.h"
#include "tests_68040.h"
#include "tests_68060.h"
#include "tests_68080.h"
#include "machine_tests/runner.h"
#include <stdio.h>

/* Declared in timing_tests.c */
int run_timing_tests(void);

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

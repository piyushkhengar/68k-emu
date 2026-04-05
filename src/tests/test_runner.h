#ifndef TEST_RUNNER_H
#define TEST_RUNNER_H

#include <stddef.h>
#include <stdint.h>
#include "cpu.h"

/* Test ROM descriptor — shared by all per-CPU test suites. */
typedef struct {
    const char    *name;
    const uint8_t *rom;
    size_t         size;
    const char    *description;
    int            max_steps;     /* 0 = default 100 */
    int            retired_after; /* 0 = works on all models;
                                     non-zero = skip when (int)model > this value.
                                     Use cpu_model_t integer values, e.g. CPU_MODEL_68040. */
} builtin_test_t;

/* Default NOP loop used when no ROM is specified on the command line. */
extern const uint8_t nop_loop[];
extern const size_t  nop_loop_size;

/* Search all per-CPU test tables for a test by name.
 * Returns NULL if not found. */
const builtin_test_t *find_test_by_name(const char *name);

/*
 * Run a test table in a given CPU mode, using check_fn for pass/fail.
 * Skips entries where retired_after != 0 && (int)model > retired_after.
 * No cycle checking is performed — this is for inherited-suite regression.
 * Leaves the CPU in 'model' state; caller must restore to 68000 afterward.
 * Returns 0 if all non-skipped tests pass, 1 if any fail.
 */
int run_suite_in_mode(const builtin_test_t *table, size_t count,
                      int (*check_fn)(size_t idx),
                      cpu_model_t model, const char *label);

/* Run the full test suite (all CPU variants + all machines).
 * speed_mhz: 0 = hyperspeed. Returns 0 if all pass, 1 if any fail. */
int run_all_tests(double speed_mhz);

#endif /* TEST_RUNNER_H */

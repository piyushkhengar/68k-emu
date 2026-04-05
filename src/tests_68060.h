/*
 * 68060-specific internal tests.
 * Kept separate from other model tests to avoid polluting those suites.
 */

#ifndef TESTS_68060_H
#define TESTS_68060_H

#include "test_runner.h"

/* Look up a 68060 built-in test by name. Returns NULL if not found. */
const builtin_test_t *find_68060_test(const char *name);

/* Return the 68060 test table (for use by inherited-suite runners). */
const builtin_test_t *get_68060_tests(size_t *out_count);

/* Pass/fail check for 68060 test at index idx. */
int check_68060_result(size_t idx);

/* Run all 68060-specific internal tests.
 * Inits the CPU as 68060, runs tests, then restores CPU to 68000.
 * Returns 0 if all pass, nonzero if any fail. */
int run_68060_tests(void);

#endif /* TESTS_68060_H */

/*
 * 68080-specific internal tests (Apollo AC68080, AMMX SIMD coprocessor).
 * Kept separate from other model tests to avoid polluting those suites.
 */

#ifndef TESTS_68080_H
#define TESTS_68080_H

#include "test_runner.h"

/* Look up a 68080 built-in test by name. Returns NULL if not found. */
const builtin_test_t *find_68080_test(const char *name);

/* Return the 68080 test table (for use by inherited-suite runners). */
const builtin_test_t *get_68080_tests(size_t *out_count);

/* Pass/fail check for 68080 test at index idx. */
int check_68080_result(size_t idx);

/* Run all 68080-specific internal tests.
 * Inits the CPU as 68080, runs tests, then restores CPU to 68000.
 * Returns 0 if all pass, nonzero if any fail. */
int run_68080_tests(void);

#endif /* TESTS_68080_H */

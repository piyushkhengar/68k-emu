/*
 * 68020-specific internal tests.
 * Kept separate from 68000/68010 tests to avoid polluting those suites.
 */

#ifndef TESTS_68020_H
#define TESTS_68020_H

#include "test_runner.h"

/* Look up a 68020 built-in test by name. Returns NULL if not found. */
const builtin_test_t *find_68020_test(const char *name);

/* Return the 68020 test table (for use by inherited-suite runners). */
const builtin_test_t *get_68020_tests(size_t *out_count);

/* Pass/fail check for 68020 test at index idx. */
int check_68020_result(size_t idx);

/* Run all 68020-specific internal tests.
 * Inits the CPU as 68020, runs tests, then restores CPU to 68000.
 * Returns 0 if all pass, nonzero if any fail. */
int run_68020_tests(void);

#endif /* TESTS_68020_H */

/*
 * 68010-specific internal tests.
 * Kept separate from 68000 tests to avoid polluting the 68000 test suite.
 */

#ifndef TESTS_68010_H
#define TESTS_68010_H

#include "test_runner.h"

/* Look up a 68010 built-in test by name. Returns NULL if not found. */
const builtin_test_t *find_68010_test(const char *name);

/* Return the 68010 test table (for use by inherited-suite runners). */
const builtin_test_t *get_68010_tests(size_t *out_count);

/* Pass/fail check for 68010 test at index idx. */
int check_68010_result(size_t idx);

/* Run all 68010-specific internal tests.
 * Inits the CPU as 68010, runs tests, then restores CPU to 68000.
 * Returns 0 if all pass, nonzero if any fail. */
int run_68010_tests(void);

#endif /* TESTS_68010_H */

#ifndef TESTS_68000_H
#define TESTS_68000_H

#include "test_runner.h"

/* Look up a 68000 built-in test by name. Returns NULL if not found. */
const builtin_test_t *find_68000_test(const char *name);

/* Return the 68000 test table (for use by inherited-suite runners). */
const builtin_test_t *get_68000_tests(size_t *out_count);

/* Pass/fail check for 68000 test at index idx. */
int check_68000_result(size_t idx);

/* Run 68000 built-in ROM tests. speed_mhz: 0 = hyperspeed. Returns 0 if all pass, 1 if any fail. */
int run_68000_tests(double speed_mhz);

#endif /* TESTS_68000_H */

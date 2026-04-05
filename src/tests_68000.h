#ifndef TESTS_68000_H
#define TESTS_68000_H

#include "test_runner.h"

/* Look up a 68000 built-in test by name. Returns NULL if not found. */
const builtin_test_t *find_68000_test(const char *name);

/* Run 68000 built-in ROM tests. speed_mhz: 0 = hyperspeed. Returns 0 if all pass, 1 if any fail. */
int run_68000_tests(double speed_mhz);

#endif /* TESTS_68000_H */

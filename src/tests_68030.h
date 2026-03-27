/*
 * 68030-specific internal tests.
 * Kept separate from 68000/68010/68020 tests to avoid polluting those suites.
 */

#ifndef TESTS_68030_H
#define TESTS_68030_H

/* Run all 68030-specific internal tests.
 * Inits the CPU as 68030, runs tests, then restores CPU to 68000.
 * Returns 0 if all pass, nonzero if any fail. */
int run_68030_tests(void);

#endif /* TESTS_68030_H */

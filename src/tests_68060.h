/*
 * 68060-specific internal tests.
 * Kept separate from other model tests to avoid polluting those suites.
 */

#ifndef TESTS_68060_H
#define TESTS_68060_H

/* Run all 68060-specific internal tests.
 * Inits the CPU as 68060, runs tests, then restores CPU to 68000.
 * Returns 0 if all pass, nonzero if any fail. */
int run_68060_tests(void);

#endif /* TESTS_68060_H */

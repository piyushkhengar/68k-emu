/*
 * 68040-specific internal tests.
 * Kept separate from 68000/68010/68020/68030 tests to avoid polluting those suites.
 */

#ifndef TESTS_68040_H
#define TESTS_68040_H

/* Run all 68040-specific internal tests.
 * Inits the CPU as 68040, runs tests, then restores CPU to 68000.
 * Returns 0 if all pass, nonzero if any fail. */
int run_68040_tests(void);

#endif /* TESTS_68040_H */

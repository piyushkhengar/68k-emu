/*
 * 68080-specific internal tests (Apollo AC68080, AMMX SIMD coprocessor).
 * Kept separate from other model tests to avoid polluting those suites.
 */

#ifndef TESTS_68080_H
#define TESTS_68080_H

/* Run all 68080-specific internal tests.
 * Inits the CPU as 68080, runs tests, then restores CPU to 68000.
 * Returns 0 if all pass, nonzero if any fail. */
int run_68080_tests(void);

#endif /* TESTS_68080_H */

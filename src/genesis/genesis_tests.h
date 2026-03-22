#ifndef GENESIS_TESTS_H
#define GENESIS_TESTS_H

/* Run Genesis hardware tests (VDP, IRQ, etc.).
 * Returns number of failures (0 = all pass). */
int run_genesis_tests(double speed_mhz);

#endif /* GENESIS_TESTS_H */

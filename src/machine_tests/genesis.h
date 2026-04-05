#ifndef MACHINE_TESTS_GENESIS_H
#define MACHINE_TESTS_GENESIS_H

/* Run all Genesis hardware tests. speed_mhz: 0 = hyperspeed. Returns 0 if all pass, 1 if any fail. */
int run_genesis_machine_tests(double speed_mhz);

#endif /* MACHINE_TESTS_GENESIS_H */

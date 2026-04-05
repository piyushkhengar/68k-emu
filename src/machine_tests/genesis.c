#include "machine_tests/genesis.h"
#include "genesis/genesis_tests.h"

int run_genesis_machine_tests(double speed_mhz)
{
    return run_genesis_tests(speed_mhz) ? 1 : 0;
}

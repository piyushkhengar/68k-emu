#include "runner.h"
#include "amiga.h"
#include "genesis.h"

int run_all_machine_tests(double speed_mhz)
{
    int failed = 0;
    if (run_genesis_machine_tests(speed_mhz)) failed = 1;
    if (run_amiga_machine_tests())            failed = 1;
    return failed;
}

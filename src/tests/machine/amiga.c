#include "amiga.h"
#include "amiga/bus_tests.h"
#include "amiga/paula_tests.h"

int run_amiga_machine_tests(void)
{
    int failed = 0;
    if (run_amiga_bus_tests()) failed = 1;
    if (run_paula_tests())     failed = 1;
    return failed;
}

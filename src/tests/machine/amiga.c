#include "amiga.h"
#include "amiga/bus_tests.h"
#include "amiga/paula_tests.h"
#include "amiga/cia_tests.h"
#include "amiga/agnus_tests.h"
#include "amiga/denise_tests.h"

int run_amiga_machine_tests(void)
{
    int failed = 0;
    if (run_amiga_bus_tests()) failed = 1;
    if (run_paula_tests())     failed = 1;
    if (run_cia_tests())       failed = 1;
    if (run_agnus_tests())     failed = 1;
    if (run_denise_tests())    failed = 1;
    return failed;
}

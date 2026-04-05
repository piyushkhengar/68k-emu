#include "system.h"
#include <string.h>

extern const system_t system_genesis;
extern const system_t system_amiga;

static const system_t *systems[] = {
    &system_genesis,
    &system_amiga,
};

#define NUM_SYSTEMS (sizeof(systems) / sizeof(systems[0]))

const system_t *system_find(const char *name)
{
    for (size_t i = 0; i < NUM_SYSTEMS; i++) {
        if (strcmp(systems[i]->name, name) == 0)
            return systems[i];
    }
    return NULL;
}

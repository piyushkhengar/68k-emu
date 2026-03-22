#ifndef SYSTEM_H
#define SYSTEM_H

#include <stdint.h>
#include <stddef.h>
#include "core/memory.h"

typedef struct {
    const char *name;
    const char *description;

    int  (*init)(const uint8_t *rom, size_t size);
    void (*reset)(void);
    void (*shutdown)(void);

    void (*run)(void);
    void (*run_headless)(int max_frames);

    const mem_bus_t *bus;
} system_t;

const system_t *system_find(const char *name);

#endif /* SYSTEM_H */

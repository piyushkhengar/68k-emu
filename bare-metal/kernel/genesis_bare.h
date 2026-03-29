#ifndef GENESIS_BARE_H
#define GENESIS_BARE_H

#include <stdint.h>
#include <stddef.h>

int  genesis_bare_init(const uint8_t *rom, size_t size);
void genesis_bare_run(int max_frames);

#endif

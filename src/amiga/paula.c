/*
 * Paula — Amiga 500 audio/disk/UART chip. (STUB — Phase 2 TDD skeleton)
 *
 * All functions are intentionally empty / return zero so that the test
 * suite compiles and every test fails (red phase).  Replace with real
 * logic once the tests are reviewed and approved.
 */

#include "paula.h"
#include <string.h>

void paula_init(paula_t *p)
{
    (void)p;
}

void paula_reset(paula_t *p)
{
    (void)p;
}

void paula_write_reg(paula_t *p, uint16_t offset, uint16_t val)
{
    (void)p;
    (void)offset;
    (void)val;
}

uint16_t paula_read_reg(const paula_t *p, uint16_t offset)
{
    (void)p;
    (void)offset;
    return 0;
}

void paula_tick(paula_t *p, uint32_t color_clocks,
                int16_t *out_left, int16_t *out_right)
{
    (void)p;
    (void)color_clocks;
    if (out_left)  *out_left  = 0;
    if (out_right) *out_right = 0;
}

#ifndef EA_H
#define EA_H

#include <stdint.h>

/*
 * 68000 Effective Address helpers.
 * Mode and reg are the 6-bit EA field: mode = (op >> 3) & 7, reg = op & 7
 * (or for dest EA: mode = (op >> 9) & 7, reg = (op >> 6) & 7)
 * Size: 1 = byte, 2 = word, 4 = long.
 */

/* Step for (An)+/-(An): A7 uses 2 for byte (word align). */
int ea_step(int reg, int size);

/* Fetch value from effective address. Returns zero-extended value. */
uint32_t ea_fetch_value(int mode, int reg, int size);

/* Store value to effective address. Value is truncated to size. */
void ea_store_value(int mode, int reg, int size, uint32_t value);

#endif /* EA_H */

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

/* Compute effective address to 32-bit address (no fetch). Returns 1 if valid, 0 if invalid.
 * Used by LEA, JMP, JSR, PEA. Invalid: Dn (0), An (1), #imm (7,4).
 * For (An)+ and -(An), uses size 4 (long) for step. */
int ea_address_no_fetch(int mode, int reg, uint32_t *addr_out);

#endif /* EA_H */

#ifndef EA_H
#define EA_H

#include <stdint.h>

/*
 * 68000 Effective Address helpers.
 * Mode and reg are the 6-bit EA field: mode = (op >> 3) & 7, reg = op & 7
 * (or for dest EA: mode = (op >> 9) & 7, reg = (op >> 6) & 7)
 * Size: 1 = byte, 2 = word, 4 = long.
 */

/* Decode EA from bits 5-0 (source EA). */
static inline int ea_mode_from_op(uint16_t op) { return (op >> 3) & 7; }
static inline int ea_reg_from_op(uint16_t op)  { return op & 7; }

/* Decode EA from bits 11-6 (destination EA, e.g. MOVE). */
static inline int ea_mode_from_op_dest(uint16_t op) { return (op >> 9) & 7; }
static inline int ea_reg_from_op_dest(uint16_t op) { return (op >> 6) & 7; }

/* Decode size from bits 7-6: 00=B(1), 01=W(2), 10/11=L(4). */
static inline int decode_size_bits_6_7(uint16_t op)
{
    int c = (op >> 6) & 3;
    return (c == 0) ? 1 : (c == 1) ? 2 : 4;
}

/* Mask for size: 0xFF, 0xFFFF, or 0xFFFFFFFF. */
static inline uint32_t size_mask(int size)
{
    return (size == 1) ? 0xFFu : (size == 2) ? 0xFFFFu : 0xFFFFFFFFu;
}

/* Common decoded EA fields (mode, reg, size, mask). Used by many instructions. */
typedef struct {
    int ea_mode;
    int ea_reg;
    int size;
    uint32_t mask;
} ea_decoded_t;

/* Populate ea_decoded_t from op; size from bits 6-7. */
static inline void ea_decode_from_op(uint16_t op, ea_decoded_t *d)
{
    d->ea_mode = ea_mode_from_op(op);
    d->ea_reg = ea_reg_from_op(op);
    d->size = decode_size_bits_6_7(op);
    d->mask = size_mask(d->size);
}

/* An (mode 1) rejection: 1 = reject. */
static inline int ea_is_an(int ea_mode) { return ea_mode == 1; }

/* An + byte: illegal for many instructions. 1 = reject. */
static inline int ea_reject_byte_an(int ea_mode, int size) { return ea_mode == 1 && size == 1; }

/* Invalid EA for JMP/JSR: Dn (0), An (1), #imm (7,4). 1 = invalid. */
static inline int ea_invalid_for_jmp_jsr(int ea_mode, int ea_reg)
{
    return ea_mode == 0 || ea_mode == 1 || (ea_mode == 7 && ea_reg == 4);
}

/* Invalid EA for LEA: Dn (0), An (1), (An)+ (3), -(An) (4), #imm (7,4). 1 = invalid. */
static inline int ea_invalid_for_lea(int ea_mode, int ea_reg)
{
    return ea_mode == 0 || ea_mode == 1 || ea_mode == 3 || ea_mode == 4 || (ea_mode == 7 && ea_reg == 4);
}

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

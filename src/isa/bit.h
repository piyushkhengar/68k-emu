#ifndef BIT_H
#define BIT_H

#include <stdint.h>

/* Bit ops: Dn form (0x01xx) and #imm form (0x08xx). */
int op_bit_dn(uint16_t op);
int op_bit_imm(uint16_t op);

/* BFxxx bitfield instructions: 0xE8C0-0xEFFF (68020+). */
int op_bitfield(uint16_t op);

#endif /* BIT_H */

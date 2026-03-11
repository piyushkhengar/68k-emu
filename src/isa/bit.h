#ifndef BIT_H
#define BIT_H

#include <stdint.h>

/* Bit ops: Dn form (0x01xx) and #imm form (0x08xx). */
int op_bit_dn(uint16_t op);
int op_bit_imm(uint16_t op);

#endif /* BIT_H */

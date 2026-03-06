#ifndef CPU_INTERNAL_H
#define CPU_INTERNAL_H

#include "cpu.h"
#include "memory.h"
#include <stdint.h>

/* Shared CPU state (defined in cpu.c) */
extern CPU cpu;

/* Fetch helpers - advance PC and return value */
uint16_t fetch16(void);
uint32_t fetch32(void);

/* Flag helpers */
void set_nz_from_val(uint32_t val, int size);
void set_nzvc_add(uint32_t result, uint32_t dest_val, uint32_t source_val);
void set_nzvc_sub(uint32_t result, uint32_t dest_val, uint32_t source_val);
/* Size-aware (1=byte, 2=word, 4=long): masks operands before setting N,Z,V,C */
void set_nzvc_add_sized(uint32_t result, uint32_t dest_val, uint32_t source_val, int size);
void set_nzvc_sub_sized(uint32_t result, uint32_t dest_val, uint32_t source_val, int size);

/* Fallback for unimplemented opcodes */
void op_unimplemented(uint16_t op);

#endif /* CPU_INTERNAL_H */

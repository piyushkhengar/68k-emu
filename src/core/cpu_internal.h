#ifndef CPU_INTERNAL_H
#define CPU_INTERNAL_H

#include "cpu.h"
#include "memory.h"
#include <stdint.h>

/* 68000 exception vector numbers */
#define ADDR_ERR_VECTOR   3
#define ILLEGAL_VECTOR    4
#define DIVIDE_BY_ZERO_VECTOR 5
#define CHK_VECTOR       6
#define TRAPV_VECTOR     7
#define PRIVILEGE_VECTOR 8
#define LINE1010_VECTOR  10
#define LINE1111_VECTOR  11

/* Shared CPU state (defined in cpu.c) */
extern CPU cpu;

/* Fetch helpers - advance PC and return value */
uint16_t fetch16(void);
uint32_t fetch32(void);

/* Flag helpers */
void set_nz_from_val(uint32_t val, int size);
void set_nzvc_add(uint32_t result, uint32_t dest_val, uint32_t source_val);
void set_nzvc_sub(uint32_t result, uint32_t dest_val, uint32_t source_val);
/* Size-aware (1=byte, 2=word, 4=long): masks operands before setting N,Z,V,C,X */
void set_nzvc_add_sized(uint32_t result, uint32_t dest_val, uint32_t source_val, int size);
void set_nzvc_sub_sized(uint32_t result, uint32_t dest_val, uint32_t source_val, int size);
/* ADDX/SUBX: Z cleared if result nonzero, unchanged otherwise */
void set_nzvc_addx_sized(uint32_t result, uint32_t dest_val, uint32_t source_val, int size);
void set_nzvc_subx_sized(uint32_t result, uint32_t dest_val, uint32_t source_val, int size);

/* Fallback for unimplemented opcodes (never returns; longjmps). Returns int for dispatch compatibility. */
int op_unimplemented(uint16_t op);

/* ILLEGAL instruction (0x4AFC): explicit vector 4. Same effect as op_unimplemented. */
int op_illegal(uint16_t op);

/* Take exception: push PC and SR, set supervisor mode, load handler from vector.
 * cycles_before_fault: cycles consumed by aborted instruction (e.g. 4 for illegal opcode fetch). */
void cpu_take_exception(int vector_num, int cycles_before_fault);

/* Returns 0 if privilege violation (takes exception); 1 if OK to proceed. */
int require_supervisor(void);

/* Stack pointer helpers: use active SP (ssp when supervisor, usp when user). Always keep a[7] in sync. */
static inline uint32_t cpu_sp(void)
{
    return (cpu.sr & 0x2000) ? cpu.ssp : cpu.usp;
}
static inline void cpu_sp_set(uint32_t v)
{
    if (cpu.sr & 0x2000)
        cpu.ssp = v;
    else
        cpu.usp = v;
    cpu.a[7] = v;
}
/* Call after any direct write to cpu.a[7] to keep ssp/usp in sync. */
static inline void sync_a7_to_sp(void)
{
    if (cpu.sr & 0x2000)
        cpu.ssp = cpu.a[7];
    else
        cpu.usp = cpu.a[7];
}

#endif /* CPU_INTERNAL_H */

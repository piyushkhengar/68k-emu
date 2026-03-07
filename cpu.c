#include "cpu.h"
#include "cpu_internal.h"
#include "move.h"
#include "alu.h"
#include "branch.h"
#include "control.h"
#include "memory.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

CPU cpu;

void cpu_init(void)
{
    memset(&cpu, 0, sizeof(cpu));
}

void cpu_reset(void)
{
    /* 68K fetches reset vector at 0x000000: PC, then SP */
    cpu.pc = mem_read32(0);
    cpu.a[7] = mem_read32(4);  /* A7 = stack pointer */
    cpu.sr = 0x2700;           /* Supervisor mode, interrupts disabled */
    cpu.halted = 0;
    cpu.cycles = 0;
}

/*
 * Fetch a word at PC and advance PC.
 * 68K is big-endian; mem_read16 already returns big-endian.
 */
uint16_t fetch16(void)
{
    uint16_t w = mem_read16(cpu.pc);
    cpu.pc += 2;
    return w;
}

uint32_t fetch32(void)
{
    uint32_t w = mem_read32(cpu.pc);
    cpu.pc += 4;
    return w;
}

/* Helper: set N,Z and clear V,C from value (size in bytes: 1,2,4) */
void set_nz_from_val(uint32_t val, int size)
{
    cpu.sr &= ~(SR_N | SR_Z | SR_V | SR_C);
    if (val == 0)
        cpu.sr |= SR_Z;
    if (size == 1 && (val & 0x80))
        cpu.sr |= SR_N;
    else if (size == 2 && (val & 0x8000))
        cpu.sr |= SR_N;
    else if (size == 4 && (val & 0x80000000))
        cpu.sr |= SR_N;
}

/* Helper: set N,Z,V,C from ADD result (dest + source = result) */
void set_nzvc_add(uint32_t result, uint32_t dest_val, uint32_t source_val)
{
    int32_t a = (int32_t)dest_val, b = (int32_t)source_val, r = (int32_t)result;
    cpu.sr &= ~(SR_N | SR_Z | SR_V | SR_C);
    if (result == 0)
        cpu.sr |= SR_Z;
    if (result & 0x80000000)
        cpu.sr |= SR_N;
    if (result < dest_val)  /* Carry out (unsigned overflow) */
        cpu.sr |= SR_C;
    if ((a > 0 && b > 0 && r < 0) || (a < 0 && b < 0 && r > 0))  /* Signed overflow */
        cpu.sr |= SR_V;
}

/* Helper: set N,Z,V,C from SUB/CMP result (dest - source = result) */
void set_nzvc_sub(uint32_t result, uint32_t dest_val, uint32_t source_val)
{
    int32_t a = (int32_t)dest_val, b = (int32_t)source_val, r = (int32_t)result;
    cpu.sr &= ~(SR_N | SR_Z | SR_V | SR_C);
    if (result == 0)
        cpu.sr |= SR_Z;
    if (result & 0x80000000)
        cpu.sr |= SR_N;
    if (dest_val < source_val)  /* Borrow (unsigned underflow) */
        cpu.sr |= SR_C;
    if ((a >= 0 && b < 0 && r < 0) || (a < 0 && b >= 0 && r > 0))  /* Signed overflow */
        cpu.sr |= SR_V;
}

/* Size-aware N,Z,V,C,X for ADD (masks operands by size before flag logic) */
void set_nzvc_add_sized(uint32_t result, uint32_t dest_val, uint32_t source_val, int size)
{
    uint32_t m = (size == 1) ? 0xFF : (size == 2) ? 0xFFFF : 0xFFFFFFFF;
    uint32_t r = result & m, d = dest_val & m, s = source_val & m;
    int32_t ar = (size == 1) ? (int32_t)(int8_t)r : (size == 2) ? (int32_t)(int16_t)r : (int32_t)r;
    int32_t ad = (size == 1) ? (int32_t)(int8_t)d : (size == 2) ? (int32_t)(int16_t)d : (int32_t)d;
    int32_t as = (size == 1) ? (int32_t)(int8_t)s : (size == 2) ? (int32_t)(int16_t)s : (int32_t)s;
    cpu.sr &= ~(SR_N | SR_Z | SR_V | SR_C | SR_X);
    if (r == 0)
        cpu.sr |= SR_Z;
    if (ar < 0)
        cpu.sr |= SR_N;
    if (r < d)  /* Carry out (unsigned) */
        cpu.sr |= SR_C | SR_X;

    if ((ad > 0 && as > 0 && ar < 0) || (ad < 0 && as < 0 && ar > 0))
        cpu.sr |= SR_V;
}

/* Size-aware N,Z,V,C,X for ADDX/SUBX: Z cleared if result nonzero, unchanged otherwise */
void set_nzvc_addx_sized(uint32_t result, uint32_t dest_val, uint32_t source_val, int size)
{
    uint32_t m = (size == 1) ? 0xFF : (size == 2) ? 0xFFFF : 0xFFFFFFFF;
    uint32_t r = result & m, d = dest_val & m, s = source_val & m;
    int32_t ar = (size == 1) ? (int32_t)(int8_t)r : (size == 2) ? (int32_t)(int16_t)r : (int32_t)r;
    int32_t ad = (size == 1) ? (int32_t)(int8_t)d : (size == 2) ? (int32_t)(int16_t)d : (int32_t)d;
    int32_t as = (size == 1) ? (int32_t)(int8_t)s : (size == 2) ? (int32_t)(int16_t)s : (int32_t)s;
    cpu.sr &= ~(SR_N | SR_V | SR_C | SR_X);
    if (r != 0)
        cpu.sr &= ~SR_Z;   /* Z cleared if nonzero */
    if (ar < 0)
        cpu.sr |= SR_N;
    if (r < d)
        cpu.sr |= SR_C | SR_X;
    if ((ad > 0 && as > 0 && ar < 0) || (ad < 0 && as < 0 && ar > 0))
        cpu.sr |= SR_V;
}

void set_nzvc_subx_sized(uint32_t result, uint32_t dest_val, uint32_t source_val, int size)
{
    uint32_t m = (size == 1) ? 0xFF : (size == 2) ? 0xFFFF : 0xFFFFFFFF;
    uint32_t r = result & m, d = dest_val & m, s = source_val & m;
    int32_t ar = (size == 1) ? (int32_t)(int8_t)r : (size == 2) ? (int32_t)(int16_t)r : (int32_t)r;
    int32_t ad = (size == 1) ? (int32_t)(int8_t)d : (size == 2) ? (int32_t)(int16_t)d : (int32_t)d;
    int32_t as = (size == 1) ? (int32_t)(int8_t)s : (size == 2) ? (int32_t)(int16_t)s : (int32_t)s;
    cpu.sr &= ~(SR_N | SR_V | SR_C | SR_X);
    if (r != 0)
        cpu.sr &= ~SR_Z;
    if (ar < 0)
        cpu.sr |= SR_N;
    if (d < s)
        cpu.sr |= SR_C | SR_X;
    if ((ad >= 0 && as < 0 && ar < 0) || (ad < 0 && as >= 0 && ar > 0))
        cpu.sr |= SR_V;
}

/* Size-aware N,Z,V,C,X for SUB/CMP */
void set_nzvc_sub_sized(uint32_t result, uint32_t dest_val, uint32_t source_val, int size)
{
    uint32_t m = (size == 1) ? 0xFF : (size == 2) ? 0xFFFF : 0xFFFFFFFF;
    uint32_t r = result & m, d = dest_val & m, s = source_val & m;
    int32_t ar = (size == 1) ? (int32_t)(int8_t)r : (size == 2) ? (int32_t)(int16_t)r : (int32_t)r;
    int32_t ad = (size == 1) ? (int32_t)(int8_t)d : (size == 2) ? (int32_t)(int16_t)d : (int32_t)d;
    int32_t as = (size == 1) ? (int32_t)(int8_t)s : (size == 2) ? (int32_t)(int16_t)s : (int32_t)s;
    cpu.sr &= ~(SR_N | SR_Z | SR_V | SR_C | SR_X);
    if (r == 0)
        cpu.sr |= SR_Z;
    if (ar < 0)
        cpu.sr |= SR_N;
    if (d < s)  /* Borrow */
        cpu.sr |= SR_C | SR_X;
    if ((ad >= 0 && as < 0 && ar < 0) || (ad < 0 && as >= 0 && ar > 0))
        cpu.sr |= SR_V;
}

void op_unimplemented(uint16_t op)
{
    fprintf(stderr, "Unimplemented opcode: 0x%04X at PC=0x%08X\n", op, cpu.pc - 2);
    cpu.halted = 1;
}

typedef void (*op_handler_fn)(uint16_t op);

/* Top-nibble dispatch table. Index = op >> 12. */
static const op_handler_fn dispatch_top[16] = {
    [0x0] = op_unimplemented,
    [0x1] = dispatch_move_b,
    [0x3] = dispatch_move_w,
    [0x2] = dispatch_move_l,
    [0x4] = dispatch_4xxx,
    [0x5] = op_unimplemented,
    [0x6] = op_bcc,
    [0x7] = op_moveq,
    [0x8] = op_unimplemented,
    [0x9] = dispatch_9xxx,
    [0xA] = op_unimplemented,
    [0xB] = dispatch_Bxxx,
    [0xC] = op_unimplemented,
    [0xD] = dispatch_add,
    [0xE] = dispatch_add,
    [0xF] = dispatch_add,
};

static void execute(uint16_t op)
{
    dispatch_top[op >> 12](op);
}

int cpu_step(void)
{
    if (cpu.halted)
        return 0;

    uint16_t op = fetch16();
    execute(op);

    /* Return 4 cycles as placeholder (NOP takes 4 cycles on 68K) */
    return 4;
}

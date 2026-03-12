#include "cpu.h"
#include "cpu_internal.h"
#include "move.h"
#include "alu.h"
#include "branch.h"
#include "control.h"
#include "immediate.h"
#include "logic.h"
#include "shift.h"
#include "memory.h"
#include "timing.h"
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

CPU cpu;

/* Used by cpu_take_exception to unwind and abort the current instruction */
static jmp_buf exception_buf;
static int exception_cycles_result;

void cpu_init(void)
{
    memset(&cpu, 0, sizeof(cpu));
}

void cpu_reset(void)
{
    /* 68K fetches reset vector at 0x000000: SP (SSP), then PC (per Motorola spec) */
    cpu.ssp = mem_read32(0);
    cpu.pc = mem_read32(4);
    cpu.usp = 0;
    /* Clear D0-D7 and A0-A6 for consistent test baseline (A7 set from SSP) */
    for (int i = 0; i < 8; i++)
        cpu.d[i] = 0;
    for (int i = 0; i < 7; i++)
        cpu.a[i] = 0;
    cpu.a[7] = cpu.ssp;
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
    int32_t dest_signed = (int32_t)dest_val, source_signed = (int32_t)source_val, result_signed = (int32_t)result;
    cpu.sr &= ~(SR_N | SR_Z | SR_V | SR_C);
    if (result == 0)
        cpu.sr |= SR_Z;
    if (result & 0x80000000)
        cpu.sr |= SR_N;
    if (result < dest_val)  /* Carry out (unsigned overflow) */
        cpu.sr |= SR_C;
    if ((dest_signed > 0 && source_signed > 0 && result_signed < 0) || (dest_signed < 0 && source_signed < 0 && result_signed > 0))  /* Signed overflow */
        cpu.sr |= SR_V;
}

/* Helper: set N,Z,V,C from SUB/CMP result (dest - source = result) */
void set_nzvc_sub(uint32_t result, uint32_t dest_val, uint32_t source_val)
{
    int32_t dest_signed = (int32_t)dest_val, source_signed = (int32_t)source_val, result_signed = (int32_t)result;
    cpu.sr &= ~(SR_N | SR_Z | SR_V | SR_C);
    if (result == 0)
        cpu.sr |= SR_Z;
    if (result & 0x80000000)
        cpu.sr |= SR_N;
    if (dest_val < source_val)  /* Borrow (unsigned underflow) */
        cpu.sr |= SR_C;
    if ((dest_signed >= 0 && source_signed < 0 && result_signed < 0) || (dest_signed < 0 && source_signed >= 0 && result_signed > 0))  /* Signed overflow */
        cpu.sr |= SR_V;
}

/* Size-aware N,Z,V,C,X for ADD (masks operands by size before flag logic) */
void set_nzvc_add_sized(uint32_t result, uint32_t dest_val, uint32_t source_val, int size)
{
    uint32_t size_mask = (size == 1) ? 0xFF : (size == 2) ? 0xFFFF : 0xFFFFFFFF;
    uint32_t result_masked = result & size_mask, dest_masked = dest_val & size_mask, source_masked = source_val & size_mask;
    int32_t result_signed = (size == 1) ? (int32_t)(int8_t)result_masked : (size == 2) ? (int32_t)(int16_t)result_masked : (int32_t)result_masked;
    int32_t dest_signed = (size == 1) ? (int32_t)(int8_t)dest_masked : (size == 2) ? (int32_t)(int16_t)dest_masked : (int32_t)dest_masked;
    int32_t source_signed = (size == 1) ? (int32_t)(int8_t)source_masked : (size == 2) ? (int32_t)(int16_t)source_masked : (int32_t)source_masked;
    cpu.sr &= ~(SR_N | SR_Z | SR_V | SR_C | SR_X);
    if (result_masked == 0)
        cpu.sr |= SR_Z;
    if (result_signed < 0)
        cpu.sr |= SR_N;
    if (result_masked < dest_masked)  /* Carry out (unsigned) */
        cpu.sr |= SR_C | SR_X;

    if ((dest_signed > 0 && source_signed > 0 && result_signed < 0) || (dest_signed < 0 && source_signed < 0 && result_signed > 0))
        cpu.sr |= SR_V;
}

/* Size-aware N,Z,V,C,X for ADDX/SUBX: Z cleared if result nonzero, unchanged otherwise */
void set_nzvc_addx_sized(uint32_t result, uint32_t dest_val, uint32_t source_val, int size)
{
    uint32_t size_mask = (size == 1) ? 0xFF : (size == 2) ? 0xFFFF : 0xFFFFFFFF;
    uint32_t result_masked = result & size_mask, dest_masked = dest_val & size_mask, source_masked = source_val & size_mask;
    int32_t result_signed = (size == 1) ? (int32_t)(int8_t)result_masked : (size == 2) ? (int32_t)(int16_t)result_masked : (int32_t)result_masked;
    int32_t dest_signed = (size == 1) ? (int32_t)(int8_t)dest_masked : (size == 2) ? (int32_t)(int16_t)dest_masked : (int32_t)dest_masked;
    int32_t source_signed = (size == 1) ? (int32_t)(int8_t)source_masked : (size == 2) ? (int32_t)(int16_t)source_masked : (int32_t)source_masked;
    cpu.sr &= ~(SR_N | SR_V | SR_C | SR_X);
    if (result_masked != 0)
        cpu.sr &= ~SR_Z;   /* Z cleared if nonzero */
    if (result_signed < 0)
        cpu.sr |= SR_N;
    if (result_masked < dest_masked)
        cpu.sr |= SR_C | SR_X;
    if ((dest_signed > 0 && source_signed > 0 && result_signed < 0) || (dest_signed < 0 && source_signed < 0 && result_signed > 0))
        cpu.sr |= SR_V;
}

void set_nzvc_subx_sized(uint32_t result, uint32_t dest_val, uint32_t source_val, int size)
{
    uint32_t size_mask = (size == 1) ? 0xFF : (size == 2) ? 0xFFFF : 0xFFFFFFFF;
    uint32_t result_masked = result & size_mask, dest_masked = dest_val & size_mask, source_masked = source_val & size_mask;
    int32_t result_signed = (size == 1) ? (int32_t)(int8_t)result_masked : (size == 2) ? (int32_t)(int16_t)result_masked : (int32_t)result_masked;
    int32_t dest_signed = (size == 1) ? (int32_t)(int8_t)dest_masked : (size == 2) ? (int32_t)(int16_t)dest_masked : (int32_t)dest_masked;
    int32_t source_signed = (size == 1) ? (int32_t)(int8_t)source_masked : (size == 2) ? (int32_t)(int16_t)source_masked : (int32_t)source_masked;
    cpu.sr &= ~(SR_N | SR_V | SR_C | SR_X);
    if (result_masked != 0)
        cpu.sr &= ~SR_Z;
    if (result_signed < 0)
        cpu.sr |= SR_N;
    if (dest_masked < source_masked)
        cpu.sr |= SR_C | SR_X;
    if ((dest_signed >= 0 && source_signed < 0 && result_signed < 0) || (dest_signed < 0 && source_signed >= 0 && result_signed > 0))
        cpu.sr |= SR_V;
}

/* Size-aware N,Z,V,C,X for SUB/CMP. affect_x: 0=CMP/CMPI/CMPA preserve X, 1=SUB/SUBI set X=C. */
void set_nzvc_sub_sized(uint32_t result, uint32_t dest_val, uint32_t source_val, int size, int affect_x)
{
    uint32_t size_mask = (size == 1) ? 0xFF : (size == 2) ? 0xFFFF : 0xFFFFFFFF;
    uint32_t result_masked = result & size_mask, dest_masked = dest_val & size_mask, source_masked = source_val & size_mask;
    int32_t result_signed = (size == 1) ? (int32_t)(int8_t)result_masked : (size == 2) ? (int32_t)(int16_t)result_masked : (int32_t)result_masked;
    int32_t dest_signed = (size == 1) ? (int32_t)(int8_t)dest_masked : (size == 2) ? (int32_t)(int16_t)dest_masked : (int32_t)dest_masked;
    int32_t source_signed = (size == 1) ? (int32_t)(int8_t)source_masked : (size == 2) ? (int32_t)(int16_t)source_masked : (int32_t)source_masked;
    uint16_t clear_mask = SR_N | SR_Z | SR_V | SR_C | (affect_x ? SR_X : 0);
    cpu.sr &= ~clear_mask;
    if (result_masked == 0)
        cpu.sr |= SR_Z;
    if (result_signed < 0)
        cpu.sr |= SR_N;
    if (dest_masked < source_masked)  /* Borrow */
        cpu.sr |= SR_C | (affect_x ? SR_X : 0);
    if ((dest_signed >= 0 && source_signed < 0 && result_signed < 0) || (dest_signed < 0 && source_signed >= 0 && result_signed > 0))
        cpu.sr |= SR_V;
}

/*
 * 68000 exception processing (format 0): push PC (4 bytes), push SR (2 bytes),
 * set supervisor mode, load handler from vector table at 0x000000.
 */
void cpu_take_exception(int vector_num, int cycles_before_fault)
{
    uint16_t saved_sr = cpu.sr;

    /* If user mode, save USP before switching */
    if (!(saved_sr & 0x2000))
        cpu.usp = cpu.a[7];

    /* Switch to supervisor mode (S bit) */
    cpu.sr |= 0x2000;

    exception_cycles_result = cycles_before_fault + exception_cycles(vector_num);

    /* Push PC (4 bytes), then SR (2 bytes) to SSP. Stack grows downward. */
    uint32_t sp = cpu.ssp;
    sp -= 4;
    mem_write32(sp, cpu.pc);
    sp -= 2;
    mem_write16(sp, saved_sr);
    cpu.ssp = sp;
    cpu.a[7] = sp;

    /* Load handler address from vector table */
    cpu.pc = mem_read32((unsigned)vector_num * 4);

    /* Unwind to cpu_step and abort the current instruction */
#if defined(__GNUC__) && defined(_WIN32)
    /* MinGW-w64 setjmp/longjmp can crash on 64-bit Windows; use GCC builtins (see e.g. MinGW-w64 bug 406). */
    __builtin_longjmp(exception_buf, 1);
#else
    longjmp(exception_buf, 1);
#endif
}

int require_supervisor(void)
{
    if (!(cpu.sr & 0x2000)) {
        cpu_take_exception(PRIVILEGE_VECTOR, 4);
        return 0;
    }
    return 1;
}

int op_unimplemented(uint16_t op)
{
    (void)op;
    /* PC was advanced by fetch16; push address of illegal instruction */
    cpu.pc -= 2;
    cpu_take_exception(ILLEGAL_VECTOR, 4);  /* 4 cycles for opcode fetch */
    return 0;  /* unreachable */
}

/* ILLEGAL (0x4AFC): intentional trap to vector 4. Same as op_unimplemented. */
int op_illegal(uint16_t op)
{
    (void)op;
    cpu.pc -= 2;
    cpu_take_exception(ILLEGAL_VECTOR, 4);
    return 0;  /* unreachable */
}

/* Line 1010 (0xAxxx): unimplemented line, vector 10. */
static int op_line1010(uint16_t op)
{
    (void)op;
    cpu.pc -= 2;
    cpu_take_exception(LINE1010_VECTOR, 4);
    return 0;  /* unreachable */
}

/* Line 1111 (0xF4xx-0xFFxx): unimplemented line, vector 11. Valid ADD in 0xF is 0xF0xx-0xF3xx only. */
static int op_line1111(uint16_t op)
{
    (void)op;
    cpu.pc -= 2;
    cpu_take_exception(LINE1111_VECTOR, 4);
    return 0;  /* unreachable */
}

typedef int (*op_handler_fn)(uint16_t op);

/* Dispatch for 0xFxxx: 0xF0-F3 = ADD, 0xF4-FF = Line 1111. */
static int dispatch_Fxxx(uint16_t op)
{
    if (((op >> 8) & 0x0F) >= 4)
        return op_line1111(op);
    return dispatch_add(op);
}

/* Top-nibble dispatch table. Index = op >> 12. */
static const op_handler_fn dispatch_top[16] = {
    [0x0] = dispatch_0xxx,
    [0x1] = dispatch_move_b,
    [0x3] = dispatch_move_w,
    [0x2] = dispatch_move_l,
    [0x4] = dispatch_4xxx,
    [0x5] = dispatch_5xxx,
    [0x6] = op_bcc,
    [0x7] = op_moveq,
    [0x8] = dispatch_8xxx,
    [0x9] = dispatch_9xxx,
    [0xA] = op_line1010,
    [0xB] = dispatch_Bxxx,
    [0xC] = dispatch_Cxxx,
    [0xD] = dispatch_add,
    [0xE] = dispatch_Exxx,
    [0xF] = dispatch_Fxxx,
};

static int execute(uint16_t op)
{
    return dispatch_top[op >> 12](op);
}

int cpu_step(void)
{
    if (cpu.halted)
        return 0;

#if defined(__GNUC__) && defined(_WIN32)
    if (__builtin_setjmp(exception_buf) != 0) {
#else
    if (setjmp(exception_buf) != 0) {
#endif
        /* Exception occurred; PC already updated to handler */
        return exception_cycles_result;
    }

    uint16_t op = fetch16();
    return execute(op);
}

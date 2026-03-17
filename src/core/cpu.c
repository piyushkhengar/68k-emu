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
int cpu_write_bus_adj = 0;

/* Used by cpu_take_exception to unwind and abort the current instruction */
static jmp_buf exception_buf;
static int exception_cycles_result;

static void (*trace_jsr_fn)(uint32_t addr);
static void (*trace_branch_to_fn)(uint32_t from_pc, uint32_t to_pc);

void cpu_set_trace_jsr(void (*fn)(uint32_t addr))
{
    trace_jsr_fn = fn;
}

void cpu_trace_jsr(uint32_t addr)
{
    if (trace_jsr_fn)
        trace_jsr_fn(addr);
}

void cpu_set_trace_branch_to(void (*fn)(uint32_t from_pc, uint32_t to_pc))
{
    trace_branch_to_fn = fn;
}

void cpu_trace_branch_to(uint32_t from_pc, uint32_t to_pc)
{
    if (trace_branch_to_fn)
        trace_branch_to_fn(from_pc, to_pc);
}

void cpu_init(void)
{
    memset(&cpu, 0, sizeof(cpu));
    trace_jsr_fn = NULL;
    trace_branch_to_fn = NULL;
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
    uint32_t mask = (size == 1) ? 0xFFu : (size == 2) ? 0xFFFFu : 0xFFFFFFFFu;
    uint32_t masked = val & mask;
    cpu.sr &= ~(SR_N | SR_Z | SR_V | SR_C);
    if (masked == 0)
        cpu.sr |= SR_Z;
    if (size == 1 && (masked & 0x80))
        cpu.sr |= SR_N;
    else if (size == 2 && (masked & 0x8000))
        cpu.sr |= SR_N;
    else if (size == 4 && (masked & 0x80000000))
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
void set_nzvc_addx_sized(uint32_t result, uint32_t dest_val, uint32_t source_val, int size, uint32_t xbit)
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
    /* Carry: sum overflows the size (xbit must be included in the check) */
    if ((uint64_t)dest_masked + (uint64_t)source_masked + xbit > (uint64_t)size_mask)
        cpu.sr |= SR_C | SR_X;
    if ((dest_signed > 0 && source_signed > 0 && result_signed < 0) || (dest_signed < 0 && source_signed < 0 && result_signed > 0))
        cpu.sr |= SR_V;
}

void set_nzvc_subx_sized(uint32_t result, uint32_t dest_val, uint32_t source_val, int size, uint32_t xbit)
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
    /* Borrow: dest < src + xbit (xbit must be included) */
    if ((uint64_t)dest_masked < (uint64_t)source_masked + xbit)
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
 * Push 14-byte address error exception frame and vector.
 * fault_addr: the odd address (full 32-bit).
 * ir:         opcode word of the faulting instruction.
 * saved_pc:   value to store in PC field of exception frame.
 * access_bits: low 5 bits encoding R/W(bit4), I/N(bit3), FC(bits2-0).
 *   0x1E = supervisor instruction fetch (R=1, I=1, FC=6)
 *   0x15 = supervisor data read         (R=1, I=0, FC=5)
 *   0x05 = supervisor data write        (R=0, I=0, FC=5)
 *   0x11 = user data read               (R=1, I=0, FC=1)
 *   0x01 = user data write              (R=0, I=0, FC=1)
 * func_code word = (ir & 0xFF00) | ((ir_lo & 0xE0) | access_bits)
 */
static void push_addr_err_frame(uint32_t fault_addr, uint16_t ir,
                                 uint32_t saved_pc, uint8_t access_bits)
{
    uint16_t func_code = (ir & 0xFF00) | (uint16_t)((ir & 0xE0) | access_bits);
    uint32_t sp = cpu.ssp;
    sp -= 2; mem_write16(sp, (uint16_t)(saved_pc & 0xFFFF));
    sp -= 2; mem_write16(sp, (uint16_t)(saved_pc >> 16));
    sp -= 2; mem_write16(sp, cpu.sr);   /* saved_sr already set (before switch) */
    sp -= 2; mem_write16(sp, ir);
    sp -= 2; mem_write16(sp, (uint16_t)(fault_addr & 0xFFFF));
    sp -= 2; mem_write16(sp, (uint16_t)(fault_addr >> 16));
    sp -= 2; mem_write16(sp, func_code);
    cpu.ssp = sp;
    cpu.a[7] = sp;
    cpu.pc = mem_read32((unsigned)ADDR_ERR_VECTOR * 4);
}

/*
 * Address error from instruction fetch at odd PC.
 * Called after any SP adjustment (e.g. RTS already incremented SP, BSR already decremented).
 * saved_pc = fault_addr - 4 (68000 prefetch pipeline accounts for 4 bytes ahead).
 */
void cpu_take_addr_err(uint32_t fault_addr, uint16_t ir)
{
    uint16_t saved_sr = cpu.sr;
    if (!(saved_sr & 0x2000))
        cpu.usp = cpu.a[7];
    cpu.sr = (saved_sr | 0x2000) & ~0x8000;  /* Set S, clear T */
    exception_cycles_result = exception_cycles(ADDR_ERR_VECTOR);
    /* FC=6 supervisor program, FC=2 user program; I=1 instruction fetch, R=1 read */
    uint8_t inst_fc = (saved_sr & 0x2000) ? 6 : 2;
    uint8_t inst_access_bits = (uint8_t)(0x10 | 0x08 | inst_fc);
    /* Restore saved_sr masked to valid bits; push_addr_err_frame uses cpu.sr for SR slot */
    cpu.sr = saved_sr & 0xA71F;
    push_addr_err_frame(fault_addr, ir, fault_addr - 4, inst_access_bits);
    cpu.sr = (saved_sr | 0x2000) & ~0x8000;

#if defined(__GNUC__) && defined(_WIN32)
    __builtin_longjmp(exception_buf, 1);
#else
    longjmp(exception_buf, 1);
#endif
}

/*
 * Address error from data access (odd address in mem_read16/32 or mem_write16/32).
 * is_read: 1 for reads, 0 for writes.
 * saved_pc = cpu.pc - 2 (PC after last instruction word fetch, minus 2).
 */
void cpu_take_addr_err_data(uint32_t fault_addr, int is_read)
{
    uint16_t saved_sr = cpu.sr;
    uint16_t ir = cpu.ir;
    if (!(saved_sr & 0x2000))
        cpu.usp = cpu.a[7];
    cpu.sr = (saved_sr | 0x2000) & ~0x8000;
    exception_cycles_result = exception_cycles(ADDR_ERR_VECTOR);
    /* Compute access_bits: R/W(bit4), I/N=0, FC(supervisor=5, user=1) */
    uint8_t fc = (saved_sr & 0x2000) ? 5 : 1;
    uint8_t access_bits = (uint8_t)((is_read ? 0x10 : 0x00) | fc);
    uint32_t saved_pc = cpu.pc - 2 + cpu_write_bus_adj;
    cpu_write_bus_adj = 0;
    /* Mask off reserved/undefined SR bits before saving in exception frame */
    cpu.sr = saved_sr & 0xA71F;
    push_addr_err_frame(fault_addr, ir, saved_pc, access_bits);
    cpu.sr = (saved_sr | 0x2000) & ~0x8000;

#if defined(__GNUC__) && defined(_WIN32)
    __builtin_longjmp(exception_buf, 1);
#else
    longjmp(exception_buf, 1);
#endif
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

    int was_trace = cpu.sr & 0x8000;

#if defined(__GNUC__) && defined(_WIN32)
    if (__builtin_setjmp(exception_buf) != 0) {
#else
    if (setjmp(exception_buf) != 0) {
#endif
        return exception_cycles_result;
    }

    cpu_write_bus_adj = 0;
    uint16_t op = fetch16();
    cpu.ir = op;
    int cycles = execute(op);

    if (was_trace && !cpu.halted) {
        uint16_t saved_sr = cpu.sr;
        if (!(saved_sr & 0x2000)) {
            cpu.usp = cpu.a[7];
            cpu.a[7] = cpu.ssp;
        }
        cpu.sr = (saved_sr | 0x2000) & ~0x8000;
        uint32_t sp = cpu.a[7] - 6;
        mem_write32(sp + 2, cpu.pc);
        mem_write16(sp, saved_sr);
        cpu.a[7] = sp;
        cpu.pc = mem_read32(TRACE_VECTOR * 4);
        cycles += exception_cycles(TRACE_VECTOR);
    }

    return cycles;
}

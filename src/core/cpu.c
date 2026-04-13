#include "cpu.h"
#include "cpu_internal.h"
#include "ea.h"
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
int cpu_ipl = 0;
int cpu_write_bus_adj = 0;
int pending_cycles = 0;

/* Used by cpu_take_exception to unwind and abort the current instruction */
static jmp_buf exception_buf;
static int exception_cycles_result;

static void (*trace_jsr_fn)(uint32_t addr);
static void (*trace_branch_to_fn)(uint32_t from_pc, uint32_t to_pc);
static void (*int_ack_fn)(int level);
static void (*reset_cb_fn)(void);

/* Top-nibble dispatch table — mutable so higher-model ISA installers can patch
 * entries at cpu_init() time without touching this file.  Populated in cpu_init(). */
typedef int (*op_handler_fn)(uint16_t op);
static op_handler_fn dispatch_top[16];

/* Forward declarations for file-static dispatch helpers defined later in this file. */
static int op_line1010(uint16_t op);
static int dispatch_Fxxx(uint16_t op);

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

void cpu_set_int_ack(void (*fn)(int level))
{
    int_ack_fn = fn;
}

void cpu_set_reset_cb(void (*fn)(void))
{
    reset_cb_fn = fn;
}

void cpu_fire_reset_cb(void)
{
    if (reset_cb_fn)
        reset_cb_fn();
}

/* Derive the cpu_features_t bitfield from a cpu_model_t. */
static cpu_features_t features_for_model(cpu_model_t model)
{
    cpu_features_t f = {0};
    /* Each case falls through to accumulate all features up to that model.
     * Exception: 68040/060 break out of 68030 to avoid inheriting has_pmove
     * (the 68040 uses MOVEC for MMU registers, not PMOVE/PFLUSH/PTEST). */
    switch (model) {
    case CPU_MODEL_68080: f.has_ammx         = 1;
                          f.has_lpstop       = 1;
                          /* has_movep intentionally NOT set: removed on 68060+ */
                          f.has_fpu          = 1;
                          f.has_mmu          = 1;
                          f.has_msp          = 1;
                          f.has_trapcc       = 1;
                          f.has_32bit_muldiv = 1;
                          f.has_bitfield     = 1;
                          f.has_32bit_addr   = 1;
                          f.has_full_ea      = 1;
                          f.has_movec        = 1;
                          f.has_vbr          = 1;
                          break;
    case CPU_MODEL_68060: f.has_lpstop       = 1;
                          /* has_movep intentionally NOT set: MOVEP removed on 68060 */
                          f.has_fpu          = 1;
                          f.has_mmu          = 1;
                          f.has_msp          = 1;
                          f.has_trapcc       = 1;
                          f.has_32bit_muldiv = 1;
                          f.has_bitfield     = 1;
                          f.has_32bit_addr   = 1;
                          f.has_full_ea      = 1;
                          f.has_movec        = 1;
                          f.has_vbr          = 1;
                          break;
    case CPU_MODEL_68040: f.has_fpu          = 1;
                          f.has_mmu          = 1;
                          /* has_pmove stays 0: 68040 uses MOVEC, not PMOVE */
                          f.has_msp          = 1;
                          f.has_trapcc       = 1;
                          f.has_32bit_muldiv = 1;
                          f.has_bitfield     = 1;
                          f.has_32bit_addr   = 1;
                          f.has_full_ea      = 1;
                          f.has_movec        = 1;
                          f.has_vbr          = 1;
                          f.has_movep        = 1;
                          break;
    case CPU_MODEL_68030: f.has_mmu          = 1;
                          f.has_pmove        = 1; /* fall through */
    case CPU_MODEL_68020: f.has_msp          = 1;
                          f.has_trapcc       = 1;
                          f.has_32bit_muldiv = 1;
                          f.has_bitfield     = 1;
                          f.has_32bit_addr   = 1;
                          f.has_full_ea      = 1; /* fall through */
    case CPU_MODEL_68010: f.has_movec        = 1;
                          f.has_vbr          = 1; /* fall through */
    case CPU_MODEL_68000: f.has_movep        = 1; break;
    }
    return f;
}

void cpu_init(cpu_model_t model)
{
    memset(&cpu, 0, sizeof(cpu));
    cpu.model    = model;
    cpu.features = features_for_model(model);
    /* cpu.vbr is already 0 after memset — correct for all models at reset. */
    cpu_ipl = 0;
    trace_jsr_fn = NULL;
    trace_branch_to_fn = NULL;
    int_ack_fn = NULL;
    reset_cb_fn = NULL;

    /* Build the mutable top-level dispatch table (68000 baseline). */
    dispatch_top[0x0] = dispatch_0xxx;
    dispatch_top[0x1] = dispatch_move_b;
    dispatch_top[0x2] = dispatch_move_l;
    dispatch_top[0x3] = dispatch_move_w;
    dispatch_top[0x4] = dispatch_4xxx;
    dispatch_top[0x5] = dispatch_5xxx;
    dispatch_top[0x6] = op_bcc;
    dispatch_top[0x7] = op_moveq;
    dispatch_top[0x8] = dispatch_8xxx;
    dispatch_top[0x9] = dispatch_9xxx;
    dispatch_top[0xA] = op_line1010;
    dispatch_top[0xB] = dispatch_Bxxx;
    dispatch_top[0xC] = dispatch_Cxxx;
    dispatch_top[0xD] = dispatch_add;
    dispatch_top[0xE] = dispatch_Exxx;
    dispatch_top[0xF] = dispatch_Fxxx;
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
    cpu.sr = SR_S | SR_I_MASK; /* Supervisor mode, interrupts disabled (level 7) */
    cpu.halted = 0;
    cpu.cycles = 0;
    /* 68010+: VBR resets to 0 on hardware reset. */
    cpu.vbr = 0;
    /* 68080: clear all AMMX E-registers on reset. */
    if (cpu.features.has_ammx)
        for (int i = 0; i < 24; i++)
            cpu.e[i] = 0;
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
    uint32_t mask = size_mask(size);
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

/* Mask+sign-extend boilerplate shared by all four sized flag helpers below. */
#define SIZED_OPERANDS(result, dest_val, source_val, size) \
    uint32_t mask = size_mask(size); \
    uint32_t result_masked = (result) & mask, dest_masked = (dest_val) & mask, source_masked = (source_val) & mask; \
    int32_t result_signed = sign_extend_sized(result_masked, size), dest_signed = sign_extend_sized(dest_masked, size), source_signed = sign_extend_sized(source_masked, size)

/* Size-aware N,Z,V,C,X for ADD (masks operands by size before flag logic) */
void set_nzvc_add_sized(uint32_t result, uint32_t dest_val, uint32_t source_val, int size)
{
    SIZED_OPERANDS(result, dest_val, source_val, size);
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

/* Size-aware N,Z,V,C,X for ADDX: Z cleared if result nonzero, unchanged otherwise. */
void set_nzvc_addx_sized(uint32_t result, uint32_t dest_val, uint32_t source_val, int size, uint32_t xbit)
{
    SIZED_OPERANDS(result, dest_val, source_val, size);
    cpu.sr &= ~(SR_N | SR_V | SR_C | SR_X);
    if (result_masked != 0)
        cpu.sr &= ~SR_Z;   /* Z cleared if nonzero; unchanged if zero */
    if (result_signed < 0)
        cpu.sr |= SR_N;
    /* Carry: sum overflows the size (xbit must be included in the check) */
    if ((uint64_t)dest_masked + (uint64_t)source_masked + xbit > (uint64_t)mask)
        cpu.sr |= SR_C | SR_X;
    if ((dest_signed > 0 && source_signed > 0 && result_signed < 0) || (dest_signed < 0 && source_signed < 0 && result_signed > 0))
        cpu.sr |= SR_V;
}

/* Size-aware N,Z,V,C,X for SUBX: Z cleared if result nonzero, unchanged otherwise. */
void set_nzvc_subx_sized(uint32_t result, uint32_t dest_val, uint32_t source_val, int size, uint32_t xbit)
{
    SIZED_OPERANDS(result, dest_val, source_val, size);
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
    SIZED_OPERANDS(result, dest_val, source_val, size);
    cpu.sr &= ~(SR_N | SR_Z | SR_V | SR_C | (affect_x ? SR_X : 0));
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
 * Push the standard exception stack frame and update SSP/A7.
 *
 * 68000: 6-byte frame — stack layout after push (low addr = SSP):
 *   [SSP+0] SR (2 bytes)
 *   [SSP+2] PC (4 bytes)
 *
 * 68010+ format-0 (short) frame: 8 bytes — stack layout after push:
 *   [SSP+0] format/vector word  (bits 15-12 = 0, bits 11-0 = vector*4)
 *   [SSP+2] PC (4 bytes)
 *   [SSP+6] SR (2 bytes)
 */
static void push_exc_frame(uint32_t pc, uint16_t saved_sr, int vector_num)
{
    uint32_t sp = cpu.ssp;
    if (cpu.features.has_vbr) {
        /* 68010+: push SR first (high address), then PC, then format word (low address). */
        sp -= 2; mem_write16(sp, saved_sr);
        sp -= 4; mem_write32(sp, pc);
        sp -= 2; mem_write16(sp, (uint16_t)(vector_num * 4));  /* format=0 */
    } else {
        /* 68000: push PC, then SR. */
        sp -= 4; mem_write32(sp, pc);
        sp -= 2; mem_write16(sp, saved_sr);
    }
    cpu.ssp = sp;
    cpu.a[7] = sp;
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
    cpu.pc = read_vector(ADDR_ERR_VECTOR);
}

/*
 * Address error from instruction fetch at odd PC.
 * Called after any SP adjustment (e.g. RTS already incremented SP, BSR already decremented).
 * saved_pc = fault_addr - 4 (68000 prefetch pipeline accounts for 4 bytes ahead).
 */
void cpu_take_addr_err(uint32_t fault_addr, uint16_t ir)
{
#ifdef MUSASHI_DIFF_MODE
    /* In diff mode address errors are suppressed so both emulators behave
     * identically on odd-address accesses (Musashi has AERR emulation off). */
    (void)fault_addr; (void)ir; return;
#endif
    uint16_t saved_sr = cpu.sr;
    if (!(saved_sr & SR_S))
        cpu.usp = cpu.a[7];
    cpu.sr = (saved_sr | SR_S) & ~SR_T;  /* Set S, clear T */
    exception_cycles_result = pending_cycles + exception_cycles(ADDR_ERR_VECTOR);
    /* FC=6 supervisor program, FC=2 user program; I=1 instruction fetch, R=1 read */
    uint8_t inst_fc = (saved_sr & SR_S) ? 6 : 2;
    uint8_t inst_access_bits = (uint8_t)(0x10 | 0x08 | inst_fc);
    /* Restore saved_sr masked to valid bits; push_addr_err_frame uses cpu.sr for SR slot */
    cpu.sr = saved_sr & SR_VALID;
    push_addr_err_frame(fault_addr, ir, fault_addr - 4, inst_access_bits);
    cpu.sr = (saved_sr | SR_S) & ~SR_T;

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
    if (!(saved_sr & SR_S))
        cpu.usp = cpu.a[7];
    cpu.sr = (saved_sr | SR_S) & ~SR_T;
    exception_cycles_result = pending_cycles + exception_cycles(ADDR_ERR_VECTOR);
    /* Compute access_bits: R/W(bit4), I/N=0, FC(supervisor=5, user=1) */
    uint8_t fc = (saved_sr & SR_S) ? 5 : 1;
    uint8_t access_bits = (uint8_t)((is_read ? 0x10 : 0x00) | fc);
    uint32_t saved_pc = cpu.pc - 2 + cpu_write_bus_adj;
    cpu_write_bus_adj = 0;
    /* Mask off reserved/undefined SR bits before saving in exception frame */
    cpu.sr = saved_sr & SR_VALID;
    push_addr_err_frame(fault_addr, ir, saved_pc, access_bits);
    cpu.sr = (saved_sr | SR_S) & ~SR_T;

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
    if (!(saved_sr & SR_S))
        cpu.usp = cpu.a[7];

    /* Switch to supervisor mode (S=1) and clear trace (T=0). */
    cpu.sr = (cpu.sr | SR_S) & ~SR_T;

    exception_cycles_result = cycles_before_fault + exception_cycles(vector_num);

    /* Push exception frame and load handler from vector table. */
    push_exc_frame(cpu.pc, saved_sr, vector_num);
    cpu.pc = read_vector(vector_num);

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
    if (!(cpu.sr & SR_S)) {
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

/* Dispatch for 0xFxxx opcodes. */
static int dispatch_Fxxx(uint16_t op)
{
    uint8_t upper = (op >> 8) & 0x0F;

    /* 68040+: FPU coprocessor instructions (0xF200-0xF3FF). */
    if (cpu.features.has_fpu && (upper == 2 || upper == 3))
        return op_fpu_dispatch(op);

    /* 68040+: CINV/CPUSH cache control (0xF400-0xF4FF). */
    if (cpu.features.has_fpu && upper == 4)
        return op_cache_dispatch(op);

    /* 68040+: MOVE16 (0xF600-0xF6FF). */
    if (cpu.features.has_fpu && upper == 6)
        return op_move16(op);

    /* 68030: PMOVE-based MMU instructions (0xF000-0xF0FF, CpID=0). */
    if (cpu.features.has_pmove && upper == 0)
        return op_mmu_dispatch(op);

    /* 68060+: LPSTOP #imm (0xF800 + 16-bit immediate). */
    if (cpu.features.has_lpstop && upper == 8 && op == 0xF800)
        return op_lpstop(op);

    /* 68080: AMMX coprocessor (cpID=7, 0xFE00-0xFFFF). */
    if (cpu.features.has_ammx && (upper == 0xE || upper == 0xF))
        return op_ammx_dispatch(op);

    /* Unimplemented line F. */
    if (upper >= 4)
        return op_line1111(op);

    /* 0xF000-0xF3FF on 68000/68010 where no FPU/MMU: falls to ADD encoding. */
    return dispatch_add(op);
}

/* Top-nibble dispatch table — mutable so that higher-model ISA installers can
 * patch in their handlers at cpu_init() time without touching this file. */
static op_handler_fn dispatch_top[16];

static int execute(uint16_t op)
{
    return dispatch_top[op >> 12](op);
}

int cpu_step(void)
{
    if (cpu.halted) {
        if (cpu_ipl == 7 || cpu_ipl > ((cpu.sr >> 8) & 7)) {
            cpu.halted = 0;
        } else {
            return 0;
        }
    }

    /* Check for pending external interrupt before instruction fetch.
     * Level 7 is non-maskable (taken even when mask is 7).
     * After taking the interrupt, call the int_ack callback (if set) so
     * the external hardware can clear its pending flag.  On a Genesis
     * this is the VDP; other systems wire their own acknowledge logic. */
    if (cpu_ipl > 0) {
        int mask = (cpu.sr >> 8) & 7;
        if (cpu_ipl > mask || cpu_ipl == 7) {
            int level = cpu_ipl;
            int vector = 24 + level;
            uint16_t saved_sr = cpu.sr;

            if (!(saved_sr & SR_S))
                cpu.usp = cpu.a[7];

            cpu.sr = (cpu.sr | SR_S) & ~SR_T;
            cpu.sr = (cpu.sr & ~SR_I_MASK) | (level << 8);
            push_exc_frame(cpu.pc, saved_sr, vector);
            cpu.pc = read_vector(vector);

            if (int_ack_fn)
                int_ack_fn(level);

            return exception_cycles(vector);
        }
    }

    int was_trace = cpu.sr & SR_T;

    pending_cycles = 0;

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
        if (!(saved_sr & SR_S))
            cpu.usp = cpu.a[7];
        cpu.sr = (saved_sr | SR_S) & ~SR_T;
        push_exc_frame(cpu.pc, saved_sr, TRACE_VECTOR);
        cpu.pc = read_vector(TRACE_VECTOR);
        cycles += exception_cycles(TRACE_VECTOR);
    }

    return cycles;
}

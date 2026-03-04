#include "cpu.h"
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
static inline uint16_t fetch16(void)
{
    uint16_t w = mem_read16(cpu.pc);
    cpu.pc += 2;
    return w;
}

static inline uint32_t fetch32(void)
{
    uint32_t w = mem_read32(cpu.pc);
    cpu.pc += 4;
    return w;
}

/* --- Instruction handlers --- */

static void op_nop(void)
{
    /* NOP: 0x4E71 - do nothing */
}

static void op_rts(void)
{
    /* RTS: 0x4E75 - pop return address from stack, jump to it */
    cpu.pc = mem_read32(cpu.a[7]);
    cpu.a[7] += 4;
}

static void op_move_l_dn_dn(uint16_t op)
{
    /* MOVE.L Ds, Dd: 0x2000 + (d<<3) + s
     * dest_reg = (op - 0x2000) >> 3
     * source_reg = (op - 0x2000) & 7
     */
    uint32_t x = op - 0x2000;
    int dest_reg = (x >> 3) & 7;
    int source_reg = x & 7;
    cpu.d[dest_reg] = cpu.d[source_reg];
}

/* MOVE.L (An), Dn: load from memory at address in An. EA: dest=Dn (mode 0) in bits 11-6, source=(An) (mode 2) in bits 5-0.
 * Dest reg = (op >> 6) & 7, addr reg = op & 7. Sets N, Z; clears V, C. */
static void op_move_l_an_dn(uint16_t op)
{
    int dest_reg = (op >> 6) & 7;
    int addr_reg = op & 7;
    uint32_t addr = cpu.a[addr_reg];
    uint32_t val = mem_read32(addr);

    cpu.d[dest_reg] = val;

    cpu.sr &= ~(SR_N | SR_Z | SR_V | SR_C);
    if (val == 0)
        cpu.sr |= SR_Z;
    if (val & 0x80000000)
        cpu.sr |= SR_N;
}

/* MOVE.L Dn, (An): store to memory at address in An. EA: source=Dn (mode 0), dest=(An) (mode 2).
 * Dest EA in bits 11-6: mode in 11-9, reg in 8-6. Source reg in bits 2-0. */
static void op_move_l_dn_an(uint16_t op)
{
    int source_reg = op & 7;
    int addr_reg = (op >> 6) & 7;
    uint32_t addr = cpu.a[addr_reg];
    uint32_t val = cpu.d[source_reg];

    mem_write32(addr, val);

    cpu.sr &= ~(SR_N | SR_Z | SR_V | SR_C);
    if (val == 0)
        cpu.sr |= SR_Z;
    if (val & 0x80000000)
        cpu.sr |= SR_N;
}

static void op_moveq(uint16_t op)
{
    /* MOVEQ #<data>, Dn: 0111 0nnn 0ddd dddd
     * dest_reg = (op >> 9) & 7
     * imm = sign-extended (op & 0xFF)
     * Sets N, Z; clears V, C; X unchanged
     */
    int dest_reg = (op >> 9) & 7;
    int32_t imm = (int8_t)(op & 0xFF);
    uint32_t result = (uint32_t)imm;

    cpu.d[dest_reg] = result;

    /* Update CCR: N, Z from result; clear V, C */
    cpu.sr &= ~(SR_N | SR_Z | SR_V | SR_C);
    if (result == 0)
        cpu.sr |= SR_Z;
    if (result & 0x80000000)
        cpu.sr |= SR_N;
}

static void op_add_l_dn_dn(uint16_t op)
{
    /* ADD.L Ds, Dd: Dd = Dd + Ds
     * Format: high byte 0xD0+8*d, low byte 0x80+s
     * dest_reg = ((op >> 8) - 0xD0) >> 3
     * source_reg = (op & 0xFF) - 0x80
     * Sets N, Z, V, C
     */
    int dest_reg = ((op >> 8) - 0xD0) >> 3;
    int source_reg = (op & 0xFF) - 0x80;

    uint32_t dest_val = cpu.d[dest_reg];
    uint32_t source_val = cpu.d[source_reg];
    uint32_t result = dest_val + source_val;

    cpu.d[dest_reg] = result;

    /* Update CCR */
    cpu.sr &= ~(SR_N | SR_Z | SR_V | SR_C);
    if (result == 0)
        cpu.sr |= SR_Z;
    if (result & 0x80000000)
        cpu.sr |= SR_N;
    if (result < dest_val)  /* Carry out (unsigned overflow) */
        cpu.sr |= SR_C;
    /* Signed overflow: both same sign, result different sign */
    {
        int32_t a = (int32_t)dest_val, b = (int32_t)source_val, r = (int32_t)result;
        if ((a > 0 && b > 0 && r < 0) || (a < 0 && b < 0 && r > 0))
            cpu.sr |= SR_V;
    }
}

static void op_sub_l_dn_dn(uint16_t op)
{
    /* SUB.L Ds, Dd: Dd = Dd - Ds
     * Format: high byte 0x90+8*dest, low byte 0x80+source (same as ADD, different base)
     * C = borrow (set when dest_val < source_val, i.e. unsigned underflow)
     */
    int dest_reg = ((op >> 8) - 0x90) >> 3;
    int source_reg = (op & 0xFF) - 0x80;

    uint32_t dest_val = cpu.d[dest_reg];
    uint32_t source_val = cpu.d[source_reg];
    uint32_t result = dest_val - source_val;

    cpu.d[dest_reg] = result;

    /* Update CCR */
    cpu.sr &= ~(SR_N | SR_Z | SR_V | SR_C);
    if (result == 0)
        cpu.sr |= SR_Z;
    if (result & 0x80000000)
        cpu.sr |= SR_N;
    if (dest_val < source_val)  /* Borrow (unsigned underflow) */
        cpu.sr |= SR_C;
    /* Signed overflow: operands different sign, result different sign from dest */
    {
        int32_t a = (int32_t)dest_val, b = (int32_t)source_val, r = (int32_t)result;
        if ((a >= 0 && b < 0 && r < 0) || (a < 0 && b >= 0 && r > 0))
            cpu.sr |= SR_V;
    }
}

static void op_cmp_l_dn_dn(uint16_t op)
{
    /* CMP.L Ds, Dd: compute Dd - Ds, set flags, don't store
     * Format: high byte 0xB0+8*dest, low byte 0x80+source (same as SUB)
     */
    int dest_reg = ((op >> 8) - 0xB0) >> 3;
    int source_reg = (op & 0xFF) - 0x80;

    uint32_t dest_val = cpu.d[dest_reg];
    uint32_t source_val = cpu.d[source_reg];
    uint32_t result = dest_val - source_val;

    /* No store - only set flags */

    /* Update CCR */
    cpu.sr &= ~(SR_N | SR_Z | SR_V | SR_C);
    if (result == 0)
        cpu.sr |= SR_Z;
    if (result & 0x80000000)
        cpu.sr |= SR_N;
    if (dest_val < source_val)
        cpu.sr |= SR_C;
    {
        int32_t a = (int32_t)dest_val, b = (int32_t)source_val, r = (int32_t)result;
        if ((a >= 0 && b < 0 && r < 0) || (a < 0 && b >= 0 && r > 0))
            cpu.sr |= SR_V;
    }
}

/* Bcc: Branch on condition. 0x6Cxx where C=condition (bits 11-8), xx=8-bit disp.
 * When disp byte is 0, fetch 16-bit extension word.
 * Condition 0 = BRA (always). BNE=0x66, BEQ=0x67.
 */
static int bcc_condition_met(uint8_t cond)
{
    uint8_t n = (cpu.sr & SR_N) ? 1 : 0;
    uint8_t z = (cpu.sr & SR_Z) ? 1 : 0;
    uint8_t v = (cpu.sr & SR_V) ? 1 : 0;
    uint8_t c = (cpu.sr & SR_C) ? 1 : 0;

    switch (cond) {
        case 0x0: return 1;           /* BRA - always */
        case 0x1: return 1;           /* BSR - handled separately in op_bcc */
        case 0x2: return !c && !z;     /* BHI */
        case 0x3: return c || z;      /* BLS */
        case 0x4: return !c;          /* BCC/BHS */
        case 0x5: return c;          /* BCS/BLO */
        case 0x6: return !z;         /* BNE */
        case 0x7: return z;          /* BEQ */
        case 0x8: return !v;          /* BVC */
        case 0x9: return v;          /* BVS */
        case 0xA: return !n;          /* BPL */
        case 0xB: return n;          /* BMI */
        case 0xC: return (n && v) || (!n && !v);  /* BGE */
        case 0xD: return (n && !v) || (!n && v);   /* BLT */
        case 0xE: return (n && v && !z) || (!n && !v && !z);  /* BGT */
        case 0xF: return z || (n && !v) || (!n && v);        /* BLE */
        default: return 0;
    }
}

static void op_bcc(uint16_t op)
{
    uint8_t cond = (op >> 8) & 0x0F;
    int32_t disp;

    if ((op & 0xFF) != 0) {
        disp = (int8_t)(op & 0xFF);  /* 8-bit displacement */
    } else {
        disp = (int16_t)fetch16();     /* 16-bit displacement */
    }

    if (cond == 0x1) {
        /* BSR: push return address (current PC) to stack, then branch */
        cpu.a[7] -= 4;
        mem_write32(cpu.a[7], cpu.pc);
        cpu.pc += disp;
    } else if (bcc_condition_met(cond)) {
        cpu.pc += disp;
    }
}

static void op_unimplemented(uint16_t op)
{
    fprintf(stderr, "Unimplemented opcode: 0x%04X at PC=0x%08X\n", op, cpu.pc - 2);
    cpu.halted = 1;
}

/* Dispatch table placeholder - you'll expand this as you add instructions */
static void execute(uint16_t op)
{
    /* Bcc: 0x6Cxx - conditional branch (BRA, BEQ, BNE, etc.) */
    if ((op & 0xF000) == 0x6000) {
        op_bcc(op);
        return;
    }

    /* MOVE.L: 0x2xxx. Check mode bits: 0x0E00=dest mode, 0x0038=source mode */
    if ((op & 0xF000) == 0x2000) {
        if ((op & 0x0E38) == 0) {
            /* MOVE.L Dn, Dn: both mode 0 */
            op_move_l_dn_dn(op);
            return;
        }
        if ((op & 0x0E00) == 0 && (op & 0x0038) == 0x10) {
            /* MOVE.L (An), Dn: dest Dn (mode 0), source (An) (mode 2) */
            op_move_l_an_dn(op);
            return;
        }
        if ((op & 0x0E00) == 0x0400 && (op & 0x0038) == 0) {
            /* MOVE.L Dn, (An): source Dn (mode 0), dest (An) (mode 2). 0x0E00 extracts mode; mode 2 gives 0x0400 */
            op_move_l_dn_an(op);
            return;
        }
    }

    /* MOVEQ #imm, Dn: 0x7xxx */
    if ((op & 0xF000) == 0x7000) {
        op_moveq(op);
        return;
    }

    /* ADD.L Dn, Dn: high byte 0xD0+8*dest, low byte 0x80+source (dest 0-7) */
    if (((op & 0xF0F8) == 0xD080) || ((op & 0xF0F8) == 0xE080) ||
        ((op & 0xF0F8) == 0xF080)) {
        op_add_l_dn_dn(op);
        return;
    }

    /* SUB.L Dn, Dn: high byte 0x90+8*dest, low byte 0x80+source */
    if ((op & 0xF0F8) == 0x9080) {
        op_sub_l_dn_dn(op);
        return;
    }

    /* CMP.L Dn, Dn: high byte 0xB0+8*dest, low byte 0x80+source */
    if ((op & 0xF0F8) == 0xB080) {
        op_cmp_l_dn_dn(op);
        return;
    }

    switch (op) {
        case 0x4E71: /* NOP */
            op_nop();
            break;
        case 0x4E75: /* RTS */
            op_rts();
            break;
        default:
            op_unimplemented(op);
            break;
    }
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

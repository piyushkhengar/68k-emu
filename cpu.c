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

static void op_bra(uint16_t op)
{
    /* BRA.S: 0x60xx - branch with 8-bit displacement */
    int8_t disp = (int8_t)(op & 0xFF);
    cpu.pc += disp;
}

static void op_unimplemented(uint16_t op)
{
    fprintf(stderr, "Unimplemented opcode: 0x%04X at PC=0x%08X\n", op, cpu.pc - 2);
    cpu.halted = 1;
}

/* Dispatch table placeholder - you'll expand this as you add instructions */
static void execute(uint16_t op)
{
    /* BRA.S: 0x60xx - 8-bit displacement */
    if ((op & 0xFF00) == 0x6000) {
        op_bra(op);
        return;
    }

    /* MOVE.L Dn, Dn: 0x2000 + (dest<<3) + source, both EAs are Dn */
    if ((op & 0xF1C0) == 0x2000) {
        op_move_l_dn_dn(op);
        return;
    }

    /* MOVEQ #imm, Dn: 0x7xxx */
    if ((op & 0xF000) == 0x7000) {
        op_moveq(op);
        return;
    }

    /* ADD.L Dn, Dn: high byte 0xD0+8*dest, low byte 0x80+source */
    if ((op & 0xF0F8) == 0xD080) {
        op_add_l_dn_dn(op);
        return;
    }

    switch (op) {
        case 0x4E71: /* NOP */
            op_nop();
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

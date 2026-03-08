#include "cpu_internal.h"
#include "control.h"
#include "timing.h"

/* NOP: no operation. 0x4E71. */
static int op_nop(uint16_t op)
{
    (void)op;
    return CYCLES_NOP;
}

/* RTS: pop return address from stack, jump to it. 0x4E75. */
static int op_rts(uint16_t op)
{
    (void)op;
    cpu.pc = mem_read32(cpu.a[7]);
    cpu.a[7] += 4;
    return CYCLES_RTS;
}

#define CYCLES_RTE  20

/* TRAP #n: software interrupt. 0x4E40-0x4E4F. Vector 32+n. */
static int op_trap(uint16_t op)
{
    int n = op & 0x0F;
    cpu_take_exception(32 + n, 4);  /* 4 cycles for opcode fetch */
    return 0;  /* unreachable */
}

/* RTE: return from exception. 0x4E73. Supervisor only. */
static int op_rte(uint16_t op)
{
    (void)op;
    /* Privilege violation if not in supervisor mode (SR bit 13) */
    if (!(cpu.sr & 0x2000)) {
        cpu_take_exception(PRIVILEGE_VECTOR, 4);
        return 0;
    }
    uint32_t sp = cpu.a[7];
    cpu.sr = mem_read16(sp);
    cpu.pc = mem_read32(sp + 2);
    cpu.a[7] = sp + 6;
    return CYCLES_RTE;
}

/* 0x4xxx: TRAP (0x4E40-0x4E4F), RTE (0x4E73), NOP (0x4E71), RTS (0x4E75). */
int dispatch_4xxx(uint16_t op)
{
    if ((op & 0xFFF0) == 0x4E40) return op_trap(op);
    if (op == 0x4E73) return op_rte(op);
    if (op == 0x4E71) return op_nop(op);
    if (op == 0x4E75) return op_rts(op);
    return op_unimplemented(op);
}

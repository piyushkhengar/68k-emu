#include "cpu_internal.h"
#include "control.h"
#include "ea.h"
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

/* NOT <ea>: one's complement. 0x46xx. An not allowed. */
static int op_not(uint16_t op)
{
    int ea_mode = (op >> 3) & 7;
    int ea_reg = op & 7;
    int size_code = (op >> 6) & 3;
    int size = (size_code == 0) ? 1 : (size_code == 1) ? 2 : 4;
    uint32_t mask = (size == 1) ? 0xFF : (size == 2) ? 0xFFFF : 0xFFFFFFFF;

    if (ea_mode == 1) {
        op_unimplemented(op);
        return 0;
    }
    uint32_t val = ea_fetch_value(ea_mode, ea_reg, size) & mask;
    uint32_t result = (~val) & mask;
    ea_store_value(ea_mode, ea_reg, size, result);
    set_nz_from_val(result, size);
    return add_sub_cycles(ea_mode, ea_reg, size, 1);
}

/* 0x4xxx: TRAP (0x4E40-0x4E4F), RTE (0x4E73), NOP (0x4E71), RTS (0x4E75), NOT (0x46xx). */
int dispatch_4xxx(uint16_t op)
{
    if ((op & 0xFFF0) == 0x4E40) return op_trap(op);
    if (op == 0x4E73) return op_rte(op);
    if (op == 0x4E71) return op_nop(op);
    if (op == 0x4E75) return op_rts(op);
    if ((op & 0xFF00) == 0x4600) return op_not(op);
    return op_unimplemented(op);
}

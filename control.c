#include "cpu_internal.h"
#include "control.h"

/* NOP: no operation. 0x4E71. */
static void op_nop(uint16_t op)
{
    (void)op;
}

/* RTS: pop return address from stack, jump to it. 0x4E75. */
static void op_rts(uint16_t op)
{
    (void)op;
    cpu.pc = mem_read32(cpu.a[7]);
    cpu.a[7] += 4;
}

/* 0x4xxx: NOP (0x4E71), RTS (0x4E75). Future: JSR, TRAP, etc. */
void dispatch_4xxx(uint16_t op)
{
    if (op == 0x4E71) { op_nop(op); return; }
    if (op == 0x4E75) { op_rts(op); return; }
    op_unimplemented(op);
}

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

/* 0x4xxx: NOP (0x4E71), RTS (0x4E75). Future: JSR, TRAP, etc. */
int dispatch_4xxx(uint16_t op)
{
    if (op == 0x4E71) return op_nop(op);
    if (op == 0x4E75) return op_rts(op);
    return op_unimplemented(op);
}

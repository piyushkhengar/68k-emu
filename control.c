#include "cpu_internal.h"
#include "control.h"

static void op_nop(uint16_t op)
{
    (void)op;
}

static void op_rts(uint16_t op)
{
    (void)op;
    cpu.pc = mem_read32(cpu.a[7]);
    cpu.a[7] += 4;
}

void dispatch_4xxx(uint16_t op)
{
    if (op == 0x4E71) { op_nop(op); return; }
    if (op == 0x4E75) { op_rts(op); return; }
    op_unimplemented(op);
}

#include "cpu_internal.h"
#include "alu.h"

static void op_add_l_dn_dn(uint16_t op)
{
    int dest_reg = ((op >> 8) - 0xD0) >> 3;
    int source_reg = (op & 0xFF) - 0x80;
    uint32_t dest_val = cpu.d[dest_reg];
    uint32_t source_val = cpu.d[source_reg];
    uint32_t result = dest_val + source_val;
    cpu.d[dest_reg] = result;
    set_nzvc_add(result, dest_val, source_val);
}

static void op_sub_l_dn_dn(uint16_t op)
{
    int dest_reg = ((op >> 8) - 0x90) >> 3;
    int source_reg = (op & 0xFF) - 0x80;
    uint32_t dest_val = cpu.d[dest_reg];
    uint32_t source_val = cpu.d[source_reg];
    uint32_t result = dest_val - source_val;
    cpu.d[dest_reg] = result;
    set_nzvc_sub(result, dest_val, source_val);
}

static void op_cmp_l_dn_dn(uint16_t op)
{
    int dest_reg = ((op >> 8) - 0xB0) >> 3;
    int source_reg = (op & 0xFF) - 0x80;
    uint32_t dest_val = cpu.d[dest_reg];
    uint32_t source_val = cpu.d[source_reg];
    uint32_t result = dest_val - source_val;
    set_nzvc_sub(result, dest_val, source_val);
}

void op_moveq(uint16_t op)
{
    int dest_reg = (op >> 9) & 7;
    int32_t imm = (int8_t)(op & 0xFF);
    uint32_t result = (uint32_t)imm;
    cpu.d[dest_reg] = result;
    set_nz_from_val(result, 4);
}

void dispatch_9xxx(uint16_t op)
{
    if ((op & 0xF0F8) == 0x9080) { op_sub_l_dn_dn(op); return; }
    op_unimplemented(op);
}

void dispatch_Bxxx(uint16_t op)
{
    if ((op & 0xF0F8) == 0xB080) { op_cmp_l_dn_dn(op); return; }
    op_unimplemented(op);
}

void dispatch_Dxxx(uint16_t op)
{
    if ((op & 0xF0F8) == 0xD080) { op_add_l_dn_dn(op); return; }
    op_unimplemented(op);
}

void dispatch_Exxx(uint16_t op)
{
    if ((op & 0xF0F8) == 0xE080) { op_add_l_dn_dn(op); return; }
    op_unimplemented(op);
}

void dispatch_Fxxx(uint16_t op)
{
    if ((op & 0xF0F8) == 0xF080) { op_add_l_dn_dn(op); return; }
    op_unimplemented(op);
}

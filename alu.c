#include "cpu_internal.h"
#include "alu.h"

/* ADD.L Dn, Dn: Dd = Dd + Ds. Format: high byte 0xD0+8*dest, low 0x80+source. Sets N,Z,V,C. */
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

/* SUB.L Dn, Dn: Dd = Dd - Ds. Format: high byte 0x90+8*dest, low 0x80+source. C = borrow. */
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

/* CMP.L Dn, Dn: compute Dd - Ds, set flags, don't store. Format: high 0xB0+8*dest, low 0x80+source. */
static void op_cmp_l_dn_dn(uint16_t op)
{
    int dest_reg = ((op >> 8) - 0xB0) >> 3;
    int source_reg = (op & 0xFF) - 0x80;
    uint32_t dest_val = cpu.d[dest_reg];
    uint32_t source_val = cpu.d[source_reg];
    uint32_t result = dest_val - source_val;
    set_nzvc_sub(result, dest_val, source_val);
}

/* MOVEQ #imm, Dn: sign-extend 8-bit immediate to 32-bit, load into Dn. Sets N,Z; clears V,C. */
void op_moveq(uint16_t op)
{
    int dest_reg = (op >> 9) & 7;
    int32_t imm = (int8_t)(op & 0xFF);
    uint32_t result = (uint32_t)imm;
    cpu.d[dest_reg] = result;
    set_nz_from_val(result, 4);
}

/* 0x9xxx: SUB.L Dn, Dn (mask 0xF0F8 == 0x9080). */
void dispatch_9xxx(uint16_t op)
{
    if ((op & 0xF0F8) == 0x9080) { op_sub_l_dn_dn(op); return; }
    op_unimplemented(op);
}

/* 0xBxxx: CMP.L Dn, Dn (mask 0xF0F8 == 0xB080). */
void dispatch_Bxxx(uint16_t op)
{
    if ((op & 0xF0F8) == 0xB080) { op_cmp_l_dn_dn(op); return; }
    op_unimplemented(op);
}

/* 0xDxxx: ADD.L Dn, Dn (mask 0xF0F8 == 0xD080). */
void dispatch_Dxxx(uint16_t op)
{
    if ((op & 0xF0F8) == 0xD080) { op_add_l_dn_dn(op); return; }
    op_unimplemented(op);
}

/* 0xExxx: ADD.L Dn, Dn (mask 0xF0F8 == 0xE080). */
void dispatch_Exxx(uint16_t op)
{
    if ((op & 0xF0F8) == 0xE080) { op_add_l_dn_dn(op); return; }
    op_unimplemented(op);
}

/* 0xFxxx: ADD.L Dn, Dn (mask 0xF0F8 == 0xF080). */
void dispatch_Fxxx(uint16_t op)
{
    if ((op & 0xF0F8) == 0xF080) { op_add_l_dn_dn(op); return; }
    op_unimplemented(op);
}

#include "cpu_internal.h"
#include "ea.h"
#include "alu.h"

/*
 * ADD/SUB/CMP using EA module. Supports all sizes (B, W, L) and EA modes
 * implemented in ea.c: Dn, An, (An), (An)+, -(An), d(An), abs.w, abs.l, d(PC), #imm.
 *
 * Encoding: bits 7-6=size, bit 9=direction, bits 11-9=Dn, bits 5-0=EA mode/reg.
 */

static int alu_size(int op)
{
    int sz = (op >> 6) & 3;
    return (sz == 0) ? 1 : (sz == 1) ? 2 : 4;
}

/* Dn register: bits 11-9 encoded as (high_byte - base) >> 3. Base: ADD 0xD0/0xE0/0xF0, SUB 0x90, CMP 0xB0 */
static int alu_dn_reg(uint16_t op, uint8_t base)
{
    return ((op >> 8) - base) >> 3;
}

static void op_add_generic(uint16_t op, int size)
{
    /* ADD base 0xD0: 0xD0=D0, 0xD8=D1, 0xE0=D2, 0xE8=D3, 0xF0=D4, etc. */
    int dn_reg = alu_dn_reg(op, 0xD0);
    int ea_mode = (op >> 3) & 7;
    int ea_reg = op & 7;
    int dir = (op >> 9) & 1;  /* 0 = <ea> to Dn, 1 = Dn to <ea> */

    if (dir == 0) {
        /* ADD <ea>, Dn */
        uint32_t src = ea_fetch_value(ea_mode, ea_reg, size);
        uint32_t dest_val = cpu.d[dn_reg];
        uint32_t result;
        if (size == 1) {
            dest_val &= 0xFF;
            result = (dest_val + src) & 0xFF;
            cpu.d[dn_reg] = (cpu.d[dn_reg] & 0xFFFFFF00) | result;
        } else if (size == 2) {
            dest_val &= 0xFFFF;
            result = (dest_val + src) & 0xFFFF;
            cpu.d[dn_reg] = (cpu.d[dn_reg] & 0xFFFF0000) | result;
        } else {
            result = dest_val + src;
            cpu.d[dn_reg] = result;
        }
        set_nzvc_add_sized(result, dest_val, src, size);
    } else {
        /* ADD Dn, <ea> */
        uint32_t src = cpu.d[dn_reg];
        uint32_t dest_val = ea_fetch_value(ea_mode, ea_reg, size);
        uint32_t result;
        if (size == 1) {
            src &= 0xFF;
            dest_val &= 0xFF;
            result = (dest_val + src) & 0xFF;
        } else if (size == 2) {
            src &= 0xFFFF;
            dest_val &= 0xFFFF;
            result = (dest_val + src) & 0xFFFF;
        } else {
            result = dest_val + src;
        }
        ea_store_value(ea_mode, ea_reg, size, result);
        set_nzvc_add_sized(result, dest_val, src, size);
    }
}

static void op_sub_generic(uint16_t op, int size)
{
    int dn_reg = alu_dn_reg(op, 0x90);
    int ea_mode = (op >> 3) & 7;
    int ea_reg = op & 7;
    int dir = (op >> 9) & 1;

    if (dir == 0) {
        /* SUB <ea>, Dn */
        uint32_t src = ea_fetch_value(ea_mode, ea_reg, size);
        uint32_t dest_val = cpu.d[dn_reg];
        uint32_t result;
        if (size == 1) {
            dest_val &= 0xFF;
            result = (dest_val - src) & 0xFF;
            cpu.d[dn_reg] = (cpu.d[dn_reg] & 0xFFFFFF00) | result;
        } else if (size == 2) {
            dest_val &= 0xFFFF;
            result = (dest_val - src) & 0xFFFF;
            cpu.d[dn_reg] = (cpu.d[dn_reg] & 0xFFFF0000) | result;
        } else {
            result = dest_val - src;
            cpu.d[dn_reg] = result;
        }
        set_nzvc_sub_sized(result, dest_val, src, size);
    } else {
        /* SUB Dn, <ea> */
        uint32_t src = cpu.d[dn_reg];
        uint32_t dest_val = ea_fetch_value(ea_mode, ea_reg, size);
        uint32_t result;
        if (size == 1) {
            src &= 0xFF;
            dest_val &= 0xFF;
            result = (dest_val - src) & 0xFF;
        } else if (size == 2) {
            src &= 0xFFFF;
            dest_val &= 0xFFFF;
            result = (dest_val - src) & 0xFFFF;
        } else {
            result = dest_val - src;
        }
        ea_store_value(ea_mode, ea_reg, size, result);
        set_nzvc_sub_sized(result, dest_val, src, size);
    }
}

/* CMP only has <ea>, Dn (no store) */
static void op_cmp_generic(uint16_t op, int size)
{
    int dn_reg = alu_dn_reg(op, 0xB0);
    int ea_mode = (op >> 3) & 7;
    int ea_reg = op & 7;

    uint32_t dest_val = cpu.d[dn_reg];
    uint32_t src = ea_fetch_value(ea_mode, ea_reg, size);
    uint32_t result;

    if (size == 1) {
        dest_val &= 0xFF;
        src &= 0xFF;
        result = (dest_val - src) & 0xFF;
    } else if (size == 2) {
        dest_val &= 0xFFFF;
        src &= 0xFFFF;
        result = (dest_val - src) & 0xFFFF;
    } else {
        result = dest_val - src;
    }
    set_nzvc_sub_sized(result, dest_val, src, size);
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

/* 0x9xxx: SUB */
void dispatch_9xxx(uint16_t op)
{
    int size = alu_size(op);
    op_sub_generic(op, size);
}

/* 0xBxxx: CMP */
void dispatch_Bxxx(uint16_t op)
{
    int size = alu_size(op);
    op_cmp_generic(op, size);
}

/* 0xDxxx: ADD */
void dispatch_Dxxx(uint16_t op)
{
    int size = alu_size(op);
    op_add_generic(op, size);
}

/* 0xExxx: ADD (same encoding as 0xD) */
void dispatch_Exxx(uint16_t op)
{
    int size = alu_size(op);
    op_add_generic(op, size);
}

/* 0xFxxx: ADD (same encoding as 0xD) */
void dispatch_Fxxx(uint16_t op)
{
    int size = alu_size(op);
    op_add_generic(op, size);
}

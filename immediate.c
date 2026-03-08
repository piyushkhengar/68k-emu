/*
 * ADDI, SUBI, CMPI (immediate) and ADDQ, SUBQ (quick) instructions.
 * ADDI/SUBI/CMPI: 0x04xx, 0x06xx, 0x0Cxx. Immediate data follows opcode.
 * ADDQ/SUBQ: 0x50xx, 0x51xx. Data 1-8 in bits 11-9 (0=8).
 */

#include "cpu_internal.h"
#include "ea.h"
#include "memory.h"
#include "timing.h"

static uint32_t fetch_imm(int size)
{
    if (size == 1) {
        uint16_t w = fetch16();
        return w & 0xFF;
    }
    if (size == 2)
        return fetch16() & 0xFFFF;
    return fetch32();
}

static int imm_alu_size(uint16_t op)
{
    int code = (op >> 6) & 3;
    return (code == 0) ? 1 : (code == 1) ? 2 : 4;
}

static uint32_t imm_size_mask(int size)
{
    return (size == 1) ? 0xFF : (size == 2) ? 0xFFFF : 0xFFFFFFFF;
}

/* ADDI/SUBI/CMPI: An not allowed as destination. */
static int imm_reject_an(uint16_t op, int ea_mode)
{
    if (ea_mode == 1) {
        op_unimplemented(op);
        return 1;
    }
    return 0;
}

/* ADDI #imm, <ea>: dest = dest + imm. 0x06xx */
static int op_addi(uint16_t op)
{
    int ea_mode = (op >> 3) & 7;
    int ea_reg = op & 7;
    int size = imm_alu_size(op);
    uint32_t mask = imm_size_mask(size);

    if (imm_reject_an(op, ea_mode))
        return 0;

    uint32_t imm = fetch_imm(size);
    uint32_t dest = ea_fetch_value(ea_mode, ea_reg, size);
    uint32_t result = (dest + imm) & mask;

    ea_store_value(ea_mode, ea_reg, size, result);
    set_nzvc_add_sized(result, dest, imm, size);
    return add_sub_cycles(ea_mode, ea_reg, size, 1) + (size == 4 ? 4 : 0);
}

/* SUBI #imm, <ea>: dest = dest - imm. 0x04xx */
static int op_subi(uint16_t op)
{
    int ea_mode = (op >> 3) & 7;
    int ea_reg = op & 7;
    int size = imm_alu_size(op);
    uint32_t mask = imm_size_mask(size);

    if (imm_reject_an(op, ea_mode))
        return 0;

    uint32_t imm = fetch_imm(size);
    uint32_t dest = ea_fetch_value(ea_mode, ea_reg, size);
    uint32_t result = (dest - imm) & mask;

    ea_store_value(ea_mode, ea_reg, size, result);
    set_nzvc_sub_sized(result, dest, imm, size);
    return add_sub_cycles(ea_mode, ea_reg, size, 1) + (size == 4 ? 4 : 0);
}

/* CMPI #imm, <ea>: compare, no store. 0x0Cxx. X not affected. */
static int op_cmpi(uint16_t op)
{
    int ea_mode = (op >> 3) & 7;
    int ea_reg = op & 7;
    int size = imm_alu_size(op);
    uint32_t mask = imm_size_mask(size);

    if (imm_reject_an(op, ea_mode))
        return 0;

    uint32_t imm = fetch_imm(size);
    uint32_t dest = ea_fetch_value(ea_mode, ea_reg, size) & mask;
    uint32_t result = (dest - imm) & mask;

    set_nzvc_sub_sized(result, dest, imm, size);
    return cmp_cycles(ea_mode, ea_reg, size) + (size == 4 ? 8 : 4);
}

/* ADDQ/SUBQ: data 1-8 in bits 11-9 (0=8). 0x50xx=ADDQ, 0x51xx=SUBQ */
static int op_addq_subq(uint16_t op, int is_sub)
{
    int data = (op >> 9) & 7;
    if (data == 0)
        data = 8;
    int ea_mode = (op >> 3) & 7;
    int ea_reg = op & 7;
    int size = imm_alu_size(op);

    if (ea_mode == 1) {
        /* Address register: W/L only, no flags, 32-bit result */
        if (size == 1) {
            op_unimplemented(op);
            return 0;
        }
        uint32_t dest = cpu.a[ea_reg];
        uint32_t result = is_sub ? dest - data : dest + data;
        cpu.a[ea_reg] = result;
        return 8;
    }

    uint32_t mask = imm_size_mask(size);
    uint32_t dest = ea_fetch_value(ea_mode, ea_reg, size);
    uint32_t result = is_sub ? (dest - data) & mask : (dest + data) & mask;

    ea_store_value(ea_mode, ea_reg, size, result);
    if (is_sub)
        set_nzvc_sub_sized(result, dest, (uint32_t)data, size);
    else
        set_nzvc_add_sized(result, dest, (uint32_t)data, size);

    if (ea_mode == 0)
        return 4;
    return 8 + ea_cycles(ea_mode, ea_reg, size) * 2;
}

/* 0x0xxx: ADDI (0x06), SUBI (0x04), CMPI (0x0C). Others -> unimplemented. */
int dispatch_0xxx(uint16_t op)
{
    int high = (op >> 8) & 0x0F;
    if (high == 0x06) return op_addi(op);
    if (high == 0x04) return op_subi(op);
    if (high == 0x0C) return op_cmpi(op);
    return op_unimplemented(op);
}

/* 0x5xxx: ADDQ (bit 8=0), SUBQ (bit 8=1). Data in bits 11-9. */
int dispatch_5xxx(uint16_t op)
{
    if ((op & 0x0100) == 0)
        return op_addq_subq(op, 0);   /* ADDQ */
    return op_addq_subq(op, 1);       /* SUBQ */
}

/*
 * ADDI, SUBI, CMPI (immediate) and ADDQ, SUBQ (quick) instructions.
 * ADDI/SUBI/CMPI: 0x04xx, 0x06xx, 0x0Cxx. Immediate data follows opcode.
 * ADDQ/SUBQ: 0x50xx, 0x51xx. Data 1-8 in bits 11-9 (0=8).
 */

#include "cpu_internal.h"
#include "branch.h"
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

/* ADDI/SUBI/CMPI: An not allowed as destination. */
static int imm_reject_an(uint16_t op, int ea_mode)
{
    if (ea_is_an(ea_mode)) {
        op_unimplemented(op);
        return 1;
    }
    return 0;
}

/* Returns 0 if rejected, 1 if OK to proceed. */
static int decode_imm(uint16_t op, ea_decoded_t *d)
{
    ea_decode_from_op(op, d);
    return imm_reject_an(op, d->ea_mode) ? 0 : 1;
}

/* ADDI #imm, <ea>: dest = dest + imm. 0x06xx */
static int op_addi(uint16_t op)
{
    ea_decoded_t d;
    if (!decode_imm(op, &d))
        return 0;

    uint32_t imm = fetch_imm(d.size);
    uint32_t dest = ea_fetch_value(d.ea_mode, d.ea_reg, d.size);
    uint32_t result = (dest + imm) & d.mask;

    ea_store_value(d.ea_mode, d.ea_reg, d.size, result);
    set_nzvc_add_sized(result, dest, imm, d.size);
    return add_sub_cycles(d.ea_mode, d.ea_reg, d.size, 1) + (d.size == 4 ? 4 : 0);
}

/* SUBI #imm, <ea>: dest = dest - imm. 0x04xx */
static int op_subi(uint16_t op)
{
    ea_decoded_t d;
    if (!decode_imm(op, &d))
        return 0;

    uint32_t imm = fetch_imm(d.size);
    uint32_t dest = ea_fetch_value(d.ea_mode, d.ea_reg, d.size);
    uint32_t result = (dest - imm) & d.mask;

    ea_store_value(d.ea_mode, d.ea_reg, d.size, result);
    set_nzvc_sub_sized(result, dest, imm, d.size);
    return add_sub_cycles(d.ea_mode, d.ea_reg, d.size, 1) + (d.size == 4 ? 4 : 0);
}

/* CMPI #imm, <ea>: compare, no store. 0x0Cxx. X not affected. */
static int op_cmpi(uint16_t op)
{
    ea_decoded_t d;
    if (!decode_imm(op, &d))
        return 0;

    uint32_t imm = fetch_imm(d.size);
    uint32_t dest = ea_fetch_value(d.ea_mode, d.ea_reg, d.size) & d.mask;
    uint32_t result = (dest - imm) & d.mask;

    set_nzvc_sub_sized(result, dest, imm, d.size);
    return cmp_cycles(d.ea_mode, d.ea_reg, d.size) + (d.size == 4 ? 8 : 4);
}

/* ORI #imm, <ea>: dest = dest | imm. 0x00xx. An not allowed. */
static int op_ori(uint16_t op)
{
    ea_decoded_t d;
    if (!decode_imm(op, &d))
        return 0;

    uint32_t imm = fetch_imm(d.size);
    uint32_t dest = ea_fetch_value(d.ea_mode, d.ea_reg, d.size);
    uint32_t result = (dest | imm) & d.mask;

    ea_store_value(d.ea_mode, d.ea_reg, d.size, result);
    set_nz_from_val(result, d.size);
    cpu.sr &= ~(SR_V | SR_C);
    return add_sub_cycles(d.ea_mode, d.ea_reg, d.size, 1) + (d.size == 4 ? 4 : 0);
}

/* ANDI #imm, <ea>: dest = dest & imm. 0x02xx. An not allowed. */
static int op_andi(uint16_t op)
{
    ea_decoded_t d;
    if (!decode_imm(op, &d))
        return 0;

    uint32_t imm = fetch_imm(d.size);
    uint32_t dest = ea_fetch_value(d.ea_mode, d.ea_reg, d.size);
    uint32_t result = (dest & imm) & d.mask;

    ea_store_value(d.ea_mode, d.ea_reg, d.size, result);
    set_nz_from_val(result, d.size);
    cpu.sr &= ~(SR_V | SR_C);
    return add_sub_cycles(d.ea_mode, d.ea_reg, d.size, 1) + (d.size == 4 ? 4 : 0);
}

/* EORI #imm, <ea>: dest = dest ^ imm. 0x0Axx. An not allowed. */
static int op_eori(uint16_t op)
{
    ea_decoded_t d;
    if (!decode_imm(op, &d))
        return 0;

    uint32_t imm = fetch_imm(d.size);
    uint32_t dest = ea_fetch_value(d.ea_mode, d.ea_reg, d.size);
    uint32_t result = (dest ^ imm) & d.mask;

    ea_store_value(d.ea_mode, d.ea_reg, d.size, result);
    set_nz_from_val(result, d.size);
    cpu.sr &= ~(SR_V | SR_C);
    return add_sub_cycles(d.ea_mode, d.ea_reg, d.size, 1) + (d.size == 4 ? 4 : 0);
}

#define CYCLES_ORI_ANDI_EORI_CCR_SR  20

/* ORI/ANDI/EORI to CCR: byte immediate, CCR = low byte of SR. 0x003C, 0x023C, 0x0A3C. */
static int op_ori_ccr(uint16_t op)
{
    (void)op;
    uint8_t imm = fetch16() & 0xFF;
    uint8_t ccr = cpu.sr & 0xFF;
    cpu.sr = (cpu.sr & 0xFF00) | (ccr | imm);
    return CYCLES_ORI_ANDI_EORI_CCR_SR;
}

static int op_andi_ccr(uint16_t op)
{
    (void)op;
    uint8_t imm = fetch16() & 0xFF;
    uint8_t ccr = cpu.sr & 0xFF;
    cpu.sr = (cpu.sr & 0xFF00) | (ccr & imm);
    return CYCLES_ORI_ANDI_EORI_CCR_SR;
}

static int op_eori_ccr(uint16_t op)
{
    (void)op;
    uint8_t imm = fetch16() & 0xFF;
    uint8_t ccr = cpu.sr & 0xFF;
    cpu.sr = (cpu.sr & 0xFF00) | (ccr ^ imm);
    return CYCLES_ORI_ANDI_EORI_CCR_SR;
}

/* ORI/ANDI/EORI to SR: word immediate, supervisor only. 0x007C, 0x027C, 0x0A7C. */
static int op_ori_sr(uint16_t op)
{
    (void)op;
    if (!require_supervisor())
        return 0;
    uint16_t imm = fetch16();
    cpu.sr = (cpu.sr | imm) & 0xFFFF;
    return CYCLES_ORI_ANDI_EORI_CCR_SR;
}

static int op_andi_sr(uint16_t op)
{
    (void)op;
    if (!require_supervisor())
        return 0;
    uint16_t imm = fetch16();
    cpu.sr = (cpu.sr & imm) & 0xFFFF;
    return CYCLES_ORI_ANDI_EORI_CCR_SR;
}

static int op_eori_sr(uint16_t op)
{
    (void)op;
    if (!require_supervisor())
        return 0;
    uint16_t imm = fetch16();
    cpu.sr = (cpu.sr ^ imm) & 0xFFFF;
    return CYCLES_ORI_ANDI_EORI_CCR_SR;
}

/* Decoded fields for ADDQ/SUBQ. An+byte rejected. */
typedef struct {
    int data;
    int ea_mode;
    int ea_reg;
    int size;
    uint32_t mask;
} addq_decoded_t;

/* Returns 0 if rejected, 1 if OK to proceed. */
static int decode_addq(uint16_t op, addq_decoded_t *d)
{
    d->data = (op >> 9) & 7;
    if (d->data == 0)
        d->data = 8;
    ea_decode_from_op(op, (ea_decoded_t *)&d->ea_mode);
    /* An + byte: illegal */
    if (d->ea_mode == 1 && d->size == 1) {
        op_unimplemented(op);
        return 0;
    }
    return 1;
}

/* ADDQ/SUBQ: data 1-8 in bits 11-9 (0=8). 0x50xx=ADDQ, 0x51xx=SUBQ */
static int op_addq_subq(uint16_t op, int is_sub)
{
    addq_decoded_t d;
    if (!decode_addq(op, &d))
        return 0;

    if (d.ea_mode == 1) {
        /* Address register: W/L only, no flags, 32-bit result */
        uint32_t dest = cpu.a[d.ea_reg];
        uint32_t result = is_sub ? dest - d.data : dest + d.data;
        cpu.a[d.ea_reg] = result;
        return 8;
    }

    uint32_t dest = ea_fetch_value(d.ea_mode, d.ea_reg, d.size);
    uint32_t result = is_sub ? (dest - d.data) & d.mask : (dest + d.data) & d.mask;

    ea_store_value(d.ea_mode, d.ea_reg, d.size, result);
    if (is_sub)
        set_nzvc_sub_sized(result, dest, (uint32_t)d.data, d.size);
    else
        set_nzvc_add_sized(result, dest, (uint32_t)d.data, d.size);

    if (d.ea_mode == 0)
        return 4;
    return 8 + ea_cycles(d.ea_mode, d.ea_reg, d.size) * 2;
}

/* 0x0xxx: ORI (0x00), ANDI (0x02), SUBI (0x04), ADDI (0x06), EORI (0x0A), CMPI (0x0C).
 * ORI/ANDI/EORI to CCR (0x3C) and SR (0x7C). */
int dispatch_0xxx(uint16_t op)
{
    int ea_field = op & 0x003F;
    int high = (op >> 8) & 0x0F;
    if (ea_field == 0x003C) {
        if (high == 0x00) return op_ori_ccr(op);
        if (high == 0x02) return op_andi_ccr(op);
        if (high == 0x0A) return op_eori_ccr(op);
        return op_unimplemented(op);  /* SUBI/ADDI/CMPI to CCR invalid */
    }
    if (ea_field == 0x007C) {
        if (high == 0x00) return op_ori_sr(op);
        if (high == 0x02) return op_andi_sr(op);
        if (high == 0x0A) return op_eori_sr(op);
        return op_unimplemented(op);  /* SUBI/ADDI/CMPI to SR invalid */
    }

    if (high == 0x00) return op_ori(op);
    if (high == 0x02) return op_andi(op);
    if (high == 0x04) return op_subi(op);
    if (high == 0x06) return op_addi(op);
    if (high == 0x0A) return op_eori(op);
    if (high == 0x0C) return op_cmpi(op);
    return op_unimplemented(op);
}

/* DBcc: 0x50C0-0x50FF. Decrement Dn (word); if condition false and Dn != -1, branch. */
static int op_dbcc(uint16_t op)
{
    uint8_t cond = (op >> 8) & 0x0F;
    int dn = op & 7;
    int32_t disp = (int16_t)fetch16();

    if (branch_condition_met(cond))
        return dbcc_cycles(0);   /* Condition true: terminate, no operation */

    uint16_t dn_val = (uint16_t)(cpu.d[dn] & 0xFFFF);
    int16_t new_val = (int16_t)(dn_val - 1);
    cpu.d[dn] = (cpu.d[dn] & 0xFFFF0000) | ((uint32_t)(uint16_t)new_val & 0xFFFF);

    if (new_val != -1) {
        cpu.pc += disp;
        return dbcc_cycles(1);
    }
    return dbcc_cycles(0);
}

/* Scc: 0x5Cxx. Set byte to 0xFF if condition true, else 0x00. An (mode 1) not allowed. */
static int op_scc(uint16_t op)
{
    int ea_mode = ea_mode_from_op(op);
    int ea_reg = ea_reg_from_op(op);
    uint8_t cond = (op >> 8) & 0x0F;

    if (ea_is_an(ea_mode)) {
        op_unimplemented(op);
        return 0;
    }

    uint8_t val = branch_condition_met(cond) ? 0xFF : 0x00;
    ea_store_value(ea_mode, ea_reg, 1, val);
    return scc_cycles(ea_mode, ea_reg);
}

/* 0x5xxx: DBcc (bits 7-3=11001), Scc (bits 7-3!=11001, size 11), ADDQ, SUBQ. */
int dispatch_5xxx(uint16_t op)
{
    if ((op & 0xF0C0) == 0x50C0) {  /* Size 11 in bits 7-6 */
        if ((op & 0x00F8) == 0x00C8)  /* DBcc: bits 7-3 = 11001 */
            return op_dbcc(op);
        return op_scc(op);             /* Scc: EA in bits 5-0 */
    }
    if ((op & 0x0100) == 0)
        return op_addq_subq(op, 0);   /* ADDQ */
    return op_addq_subq(op, 1);       /* SUBQ */
}

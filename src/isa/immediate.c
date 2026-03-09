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

/* Decoded fields for ADDI/SUBI/CMPI. */
typedef struct {
    int ea_mode;
    int ea_reg;
    int size;
    uint32_t mask;
} imm_decoded_t;

/* Returns 0 if rejected, 1 if OK to proceed. */
static int imm_decode(uint16_t op, imm_decoded_t *d)
{
    d->ea_mode = (op >> 3) & 7;
    d->ea_reg = op & 7;
    d->size = imm_alu_size(op);
    d->mask = imm_size_mask(d->size);
    return imm_reject_an(op, d->ea_mode) ? 0 : 1;
}

/* ADDI #imm, <ea>: dest = dest + imm. 0x06xx */
static int op_addi(uint16_t op)
{
    imm_decoded_t d;
    if (!imm_decode(op, &d))
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
    imm_decoded_t d;
    if (!imm_decode(op, &d))
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
    imm_decoded_t d;
    if (!imm_decode(op, &d))
        return 0;

    uint32_t imm = fetch_imm(d.size);
    uint32_t dest = ea_fetch_value(d.ea_mode, d.ea_reg, d.size) & d.mask;
    uint32_t result = (dest - imm) & d.mask;

    set_nzvc_sub_sized(result, dest, imm, d.size);
    return cmp_cycles(d.ea_mode, d.ea_reg, d.size) + (d.size == 4 ? 8 : 4);
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
static int addq_decode(uint16_t op, addq_decoded_t *d)
{
    d->data = (op >> 9) & 7;
    if (d->data == 0)
        d->data = 8;
    d->ea_mode = (op >> 3) & 7;
    d->ea_reg = op & 7;
    d->size = imm_alu_size(op);
    d->mask = imm_size_mask(d->size);
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
    if (!addq_decode(op, &d))
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

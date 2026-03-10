/*
 * AND, OR, EOR logical operations.
 * AND: 0xC0xx (Dn to EA), 0xC1xx (EA to Dn)
 * OR:  0x80xx (Dn to EA), 0x81xx (EA to Dn)
 * EOR: 0xB0xx, 0xB1xx, 0xB2xx (Dn to EA only)
 * MULU/MULS use opmode 011/111 in 0xC; DIVU/DIVS in 0x8.
 */

#include "cpu_internal.h"
#include "ea.h"
#include "logic.h"
#include "timing.h"

static int logic_size(int op)
{
    int code = (op >> 6) & 3;
    return (code == 0) ? 1 : (code == 1) ? 2 : 4;
}

static uint32_t logic_size_mask(int size)
{
    return (size == 1) ? 0xFF : (size == 2) ? 0xFFFF : 0xFFFFFFFF;
}

/* Byte ops cannot use An (mode 1). */
static int logic_reject_byte_an(uint16_t op, int ea_mode, int size)
{
    if (ea_mode == 1 && size == 1) {
        op_unimplemented(op);
        return 1;
    }
    return 0;
}

static void logic_store_dn(int reg, uint32_t result, int size)
{
    if (size == 1)
        cpu.d[reg] = (cpu.d[reg] & 0xFFFFFF00) | (result & 0xFF);
    else if (size == 2)
        cpu.d[reg] = (cpu.d[reg] & 0xFFFF0000) | (result & 0xFFFF);
    else
        cpu.d[reg] = result;
}

/* Decoded fields for AND/OR/EOR. */
typedef struct {
    int dn_reg;
    int ea_mode;
    int ea_reg;
    int size;
    uint32_t mask;
} logic_decoded_t;

/* Returns 0 if rejected, 1 if OK to proceed. */
static int decode_logic(uint16_t op, logic_decoded_t *d)
{
    d->dn_reg = (op >> 9) & 7;
    d->ea_mode = (op >> 3) & 7;
    d->ea_reg = op & 7;
    d->size = logic_size(op);
    d->mask = logic_size_mask(d->size);
    return logic_reject_byte_an(op, d->ea_mode, d->size) ? 0 : 1;
}

typedef uint32_t (*logic_binop_fn)(uint32_t a, uint32_t b);

static uint32_t logic_and(uint32_t a, uint32_t b) { return a & b; }
static uint32_t logic_or(uint32_t a, uint32_t b) { return a | b; }

/* AND/OR: dir=0 EA to Dn, dir=1 Dn to EA. */
static int op_logic_binop(uint16_t op, logic_binop_fn fn)
{
    logic_decoded_t d;
    if (!decode_logic(op, &d))
        return 0;

    int dir = (op >> 8) & 1;  /* bit 8: 0=Dn to EA, 1=EA to Dn */
    uint32_t src, dest_val, result;

    if (dir == 0) {
        /* <ea>, Dn */
        src = ea_fetch_value(d.ea_mode, d.ea_reg, d.size) & d.mask;
        dest_val = cpu.d[d.dn_reg] & d.mask;
        result = fn(dest_val, src) & d.mask;
        logic_store_dn(d.dn_reg, result, d.size);
    } else {
        /* Dn, <ea> */
        src = cpu.d[d.dn_reg] & d.mask;
        dest_val = ea_fetch_value(d.ea_mode, d.ea_reg, d.size) & d.mask;
        result = fn(dest_val, src) & d.mask;
        ea_store_value(d.ea_mode, d.ea_reg, d.size, result);
    }

    set_nz_from_val(result, d.size);
    return add_sub_cycles(d.ea_mode, d.ea_reg, d.size, dir);
}

static int op_and_generic(uint16_t op)
{
    return op_logic_binop(op, logic_and);
}

static int op_or_generic(uint16_t op)
{
    return op_logic_binop(op, logic_or);
}

/* EOR: Dn to EA only. result = ea_val ^ Dn. When EA is Dn, preserve upper bits. */
int op_eor(uint16_t op)
{
    logic_decoded_t d;
    if (!decode_logic(op, &d))
        return 0;

    uint32_t ea_val = ea_fetch_value(d.ea_mode, d.ea_reg, d.size) & d.mask;
    uint32_t dn_val = cpu.d[d.dn_reg] & d.mask;
    uint32_t result = (ea_val ^ dn_val) & d.mask;

    if (d.ea_mode == 0)
        logic_store_dn(d.ea_reg, result, d.size);
    else
        ea_store_value(d.ea_mode, d.ea_reg, d.size, result);
    set_nz_from_val(result, d.size);
    return add_sub_cycles(d.ea_mode, d.ea_reg, d.size, 1);
}

/* 0x8xxx: OR. DIVU/DIVS use opmode 011/111 -> unimplemented. */
int dispatch_8xxx(uint16_t op)
{
    int opmode = (op >> 6) & 7;
    if (opmode == 3 || opmode == 7)
        return op_unimplemented(op);
    return op_or_generic(op);
}

/* 0xCxxx: AND. MULU/MULS use opmode 011/111 -> unimplemented. */
int dispatch_Cxxx(uint16_t op)
{
    int opmode = (op >> 6) & 7;
    if (opmode == 3 || opmode == 7)
        return op_unimplemented(op);
    return op_and_generic(op);
}

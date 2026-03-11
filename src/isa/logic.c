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
#include "memory.h"
#include "timing.h"

/* EXG: swap two 32-bit registers. Opmode 8=Dx,Dy 9=Ax,Ay 17=Dx,Ay. */
static int op_exg(uint16_t op)
{
    int opmode = (op >> 3) & 0x1F;
    int rx = (op >> 9) & 7;
    int ry = op & 7;
    uint32_t tmp;

    if (opmode == 0x08) {
        tmp = cpu.d[rx];
        cpu.d[rx] = cpu.d[ry];
        cpu.d[ry] = tmp;
    } else if (opmode == 0x09) {
        tmp = cpu.a[rx];
        cpu.a[rx] = cpu.a[ry];
        cpu.a[ry] = tmp;
        if (rx == 7 || ry == 7)
            sync_a7_to_sp();
    } else if (opmode == 0x11) {
        tmp = cpu.d[rx];
        cpu.d[rx] = cpu.a[ry];
        cpu.a[ry] = tmp;
    } else {
        return op_unimplemented(op);
    }
    return exg_cycles();
}

/* BCD add: dest + src + X. Returns result, sets *carry_out. */
static uint8_t bcd_add_byte(uint8_t dest, uint8_t src, uint8_t x_in, uint8_t *carry_out)
{
    uint8_t lo = (dest & 0x0F) + (src & 0x0F) + x_in;
    uint8_t hi_carry = (lo >= 10) ? 1 : 0;
    if (lo >= 10) lo -= 10;
    uint8_t hi = (dest >> 4) + (src >> 4) + hi_carry;
    *carry_out = (hi >= 10) ? 1 : 0;
    if (hi >= 10) hi -= 10;
    return (hi << 4) | lo;
}

/* BCD subtract: dest - src - X. Returns result, sets *borrow_out. */
static uint8_t bcd_sub_byte(uint8_t dest, uint8_t src, uint8_t x_in, uint8_t *borrow_out)
{
    int lo = (int)(dest & 0x0F) - (int)(src & 0x0F) - (int)x_in;
    uint8_t hi_borrow = (lo < 0) ? 1 : 0;
    if (lo < 0) lo += 10;
    int hi = (int)(dest >> 4) - (int)(src >> 4) - (int)hi_borrow;
    *borrow_out = (hi < 0) ? 1 : 0;
    if (hi < 0) hi += 10;
    return (uint8_t)((hi << 4) | lo);
}

/* ABCD/SBCD: shared decode and structure. RM=0: Dy,Dx. RM=1: -(Ay),-(Ax). */
static int op_bcd_math(uint16_t op, int is_add)
{
    int rm = (op >> 3) & 1;
    int rx = (op >> 9) & 7;
    int ry = op & 7;
    uint8_t x_in = (cpu.sr & SR_X) ? 1 : 0;
    uint8_t carry_borrow;

    if (rm == 0) {
        uint8_t src = (uint8_t)(cpu.d[ry] & 0xFF);
        uint8_t dest = (uint8_t)(cpu.d[rx] & 0xFF);
        uint8_t result = is_add ? bcd_add_byte(dest, src, x_in, &carry_borrow)
                               : bcd_sub_byte(dest, src, x_in, &carry_borrow);
        cpu.d[rx] = (cpu.d[rx] & 0xFFFFFF00) | result;
        cpu.sr &= ~(SR_N | SR_V | SR_C | SR_X);
        if (carry_borrow) cpu.sr |= SR_C | SR_X;
        if (result != 0) cpu.sr &= ~SR_Z;
        return abcd_sbcd_cycles(0);
    } else {
        cpu.a[ry] -= ea_step(ry, 1);
        cpu.a[rx] -= ea_step(rx, 1);
        if (rx == 7 || ry == 7)
            sync_a7_to_sp();
        uint8_t src = (uint8_t)mem_read8(cpu.a[ry]);
        uint8_t dest = (uint8_t)mem_read8(cpu.a[rx]);
        uint8_t result = is_add ? bcd_add_byte(dest, src, x_in, &carry_borrow)
                               : bcd_sub_byte(dest, src, x_in, &carry_borrow);
        mem_write8(cpu.a[rx], result);
        cpu.sr &= ~(SR_N | SR_V | SR_C | SR_X);
        if (carry_borrow) cpu.sr |= SR_C | SR_X;
        if (result != 0) cpu.sr &= ~SR_Z;
        return abcd_sbcd_cycles(1);
    }
}

static int op_abcd(uint16_t op) { return op_bcd_math(op, 1); }
static int op_sbcd(uint16_t op) { return op_bcd_math(op, 0); }

/* NBCD <ea>: 0 - dest - X (BCD). 0x4800-0x483F. EA in bits 5-0: Dn (mode 0) or -(An) (mode 4). */
int op_nbcd(uint16_t op)
{
    int ea_mode = ea_mode_from_op(op);
    int ea_reg = ea_reg_from_op(op);
    uint8_t x_in = (cpu.sr & SR_X) ? 1 : 0;
    uint8_t borrow;

    if (ea_mode == 0) {
        uint8_t dest = (uint8_t)(cpu.d[ea_reg] & 0xFF);
        uint8_t result = bcd_sub_byte(0, dest, x_in, &borrow);
        cpu.d[ea_reg] = (cpu.d[ea_reg] & 0xFFFFFF00) | result;
        cpu.sr &= ~(SR_N | SR_V | SR_C | SR_X);
        if (borrow) cpu.sr |= SR_C | SR_X;
        if (result != 0) cpu.sr &= ~SR_Z;
        return nbcd_cycles(0);
    } else if (ea_mode == 4) {
        cpu.a[ea_reg] -= ea_step(ea_reg, 1);
        if (ea_reg == 7)
            sync_a7_to_sp();
        uint8_t dest = (uint8_t)mem_read8(cpu.a[ea_reg]);
        uint8_t result = bcd_sub_byte(0, dest, x_in, &borrow);
        mem_write8(cpu.a[ea_reg], result);
        cpu.sr &= ~(SR_N | SR_V | SR_C | SR_X);
        if (borrow) cpu.sr |= SR_C | SR_X;
        if (result != 0) cpu.sr &= ~SR_Z;
        return nbcd_cycles(1);
    } else {
        return op_unimplemented(op);
    }
}

/* Byte ops cannot use An (mode 1). */
static int logic_reject_byte_an(uint16_t op, int ea_mode, int size)
{
    if (ea_reject_byte_an(ea_mode, size)) {
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
    ea_decode_from_op(op, (ea_decoded_t *)&d->ea_mode);
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

/* MUL/DIV: An (mode 1) not allowed as source. */
static int mul_div_reject_an(uint16_t op, int ea_mode)
{
    if (ea_is_an(ea_mode)) {
        op_unimplemented(op);
        return 1;
    }
    return 0;
}

/* Decoded fields for MULU/MULS/DIVU/DIVS. All use <ea>, Dn, word source. */
typedef struct {
    int dn_reg;
    int ea_mode;
    int ea_reg;
} mul_div_decoded_t;

/* Returns 0 if rejected, 1 if OK to proceed. */
static int decode_mul_div(uint16_t op, mul_div_decoded_t *d)
{
    d->dn_reg = (op >> 9) & 7;
    d->ea_mode = ea_mode_from_op(op);
    d->ea_reg = ea_reg_from_op(op);
    return mul_div_reject_an(op, d->ea_mode) ? 0 : 1;
}

/* MULU.W <ea>, Dn: 16x16 -> 32 unsigned. Source=EA, multiplicand=Dn low word. */
static int op_mulu(uint16_t op)
{
    mul_div_decoded_t d;
    if (!decode_mul_div(op, &d))
        return 0;

    uint32_t src = ea_fetch_value(d.ea_mode, d.ea_reg, 2) & 0xFFFF;
    uint32_t mult = cpu.d[d.dn_reg] & 0xFFFF;
    uint32_t result = src * mult;

    cpu.d[d.dn_reg] = result;
    cpu.sr &= ~(SR_N | SR_Z | SR_V | SR_C);
    set_nz_from_val(result, 4);
    return mul_cycles(d.ea_mode, d.ea_reg);
}

/* MULS.W <ea>, Dn: 16x16 -> 32 signed. */
static int op_muls(uint16_t op)
{
    mul_div_decoded_t d;
    if (!decode_mul_div(op, &d))
        return 0;

    int32_t src = (int32_t)(int16_t)(ea_fetch_value(d.ea_mode, d.ea_reg, 2) & 0xFFFF);
    int32_t mult = (int32_t)(int16_t)(cpu.d[d.dn_reg] & 0xFFFF);
    uint32_t result = (uint32_t)(int32_t)(src * mult);

    cpu.d[d.dn_reg] = result;
    cpu.sr &= ~(SR_N | SR_Z | SR_V | SR_C);
    set_nz_from_val(result, 4);
    return mul_cycles(d.ea_mode, d.ea_reg);
}

/* DIVU.W <ea>, Dn: 32/16 -> 16q:16r. Dividend=Dn, divisor=EA. */
static int op_divu(uint16_t op)
{
    mul_div_decoded_t d;
    if (!decode_mul_div(op, &d))
        return 0;

    uint32_t divisor = ea_fetch_value(d.ea_mode, d.ea_reg, 2) & 0xFFFF;
    if (divisor == 0) {
        cpu.pc -= 2;
        cpu_take_exception(DIVIDE_BY_ZERO_VECTOR, 4);
        return 0;
    }

    uint32_t dividend = cpu.d[d.dn_reg];
    uint32_t quotient = dividend / divisor;
    if (quotient > 0xFFFF) {
        cpu.sr |= SR_V;
        cpu.sr &= ~(SR_N | SR_Z | SR_C);
        return div_cycles(d.ea_mode, d.ea_reg, 0);
    }

    uint32_t remainder = dividend % divisor;
    cpu.d[d.dn_reg] = (remainder << 16) | (quotient & 0xFFFF);
    cpu.sr &= ~(SR_N | SR_Z | SR_V | SR_C);
    set_nz_from_val(quotient & 0xFFFF, 2);
    return div_cycles(d.ea_mode, d.ea_reg, 0);
}

/* DIVS.W <ea>, Dn: 32/16 -> 16q:16r signed. */
static int op_divs(uint16_t op)
{
    mul_div_decoded_t d;
    if (!decode_mul_div(op, &d))
        return 0;

    uint32_t div_raw = ea_fetch_value(d.ea_mode, d.ea_reg, 2) & 0xFFFF;
    if (div_raw == 0) {
        cpu.pc -= 2;
        cpu_take_exception(DIVIDE_BY_ZERO_VECTOR, 4);
        return 0;
    }

    int32_t divisor = (int32_t)(int16_t)div_raw;
    int32_t dividend = (int32_t)cpu.d[d.dn_reg];
    int32_t quotient = dividend / divisor;

    if (quotient > 32767 || quotient < -32768) {
        cpu.sr |= SR_V;
        cpu.sr &= ~(SR_N | SR_Z | SR_C);
        return div_cycles(d.ea_mode, d.ea_reg, 1);
    }

    int32_t remainder = dividend % divisor;
    uint32_t result = ((uint32_t)(uint16_t)remainder << 16) | ((uint32_t)(uint16_t)quotient & 0xFFFF);
    cpu.d[d.dn_reg] = result;
    cpu.sr &= ~(SR_N | SR_Z | SR_V | SR_C);
    set_nz_from_val((uint32_t)(uint16_t)quotient, 2);
    return div_cycles(d.ea_mode, d.ea_reg, 1);
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

/* 0x8xxx: OR. SBCD, DIVU (opmode 3), DIVS (opmode 7). */
int dispatch_8xxx(uint16_t op)
{
    if ((op & 0xF1F0) == 0x8100)
        return op_sbcd(op);
    int opmode = (op >> 6) & 7;
    if (opmode == 3)
        return op_divu(op);
    if (opmode == 7)
        return op_divs(op);
    return op_or_generic(op);
}

/* 0xCxxx: EXG, ABCD, AND. MULU (opmode 3), MULS (opmode 7). */
int dispatch_Cxxx(uint16_t op)
{
    int opmode = (op >> 3) & 0x1F;
    if ((op & 0xF100) == 0xC100 && (opmode == 0x08 || opmode == 0x09 || opmode == 0x11))
        return op_exg(op);
    if ((op & 0xF1F0) == 0xC100)
        return op_abcd(op);
    int om = (op >> 6) & 7;
    if (om == 3)
        return op_mulu(op);
    if (om == 7)
        return op_muls(op);
    return op_and_generic(op);
}

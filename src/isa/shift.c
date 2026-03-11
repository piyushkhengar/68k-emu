/*
 * 68000 shift and rotate: ASL, ASR, LSL, LSR, ROL, ROR, ROXL, ROXR.
 * Register format: 1110 Ctt Ss dr i/r 0 oo rrr
 * Memory format: 1110 000o dr 11 MODE REG (word only, count=1)
 */

#include "cpu_internal.h"
#include "ea.h"
#include "alu.h"
#include "memory.h"
#include "timing.h"

/* Register format: 1110 Ctt Ss dr i/r 0 oo rrr. Ss in bits 8-7. */
static int shift_size(int op)
{
    int c = (op >> 7) & 3;
    return (c == 0) ? 1 : (c == 1) ? 2 : 4;
}

static int shift_count_imm(int op)
{
    int c = (op >> 9) & 7;
    return (c == 0) ? 8 : c;
}

static int shift_count_reg(uint16_t op)
{
    int r = (op >> 9) & 7;
    return (int)(cpu.d[r] & 63);
}

static int shift_dest_reg(uint16_t op)
{
    return op & 7;
}

static int shift_direction(uint16_t op)
{
    return (op >> 6) & 1;  /* 0=right, 1=left */
}

/* op type: oo (bits 4-3) + dr (bit 6). 0=ASR,1=ASL,2=LSR,3=LSL,4=ROR,5=ROL,6=ROXR,7=ROXL */
static int shift_op_type(uint16_t op)
{
    int oo = (op >> 3) & 3;
    int dr = (op >> 6) & 1;
    return (oo << 1) | dr;
}

/* Is this a memory shift? Format: 1110 000o dr 11 MODE REG. Bits 7-6 must be 11. */
static int is_memory_shift(uint16_t op)
{
    return (op & 0xC0) == 0xC0;  /* bits 7-6 = 11 */
}

static uint32_t shift_mask(int size)
{
    return (size == 1) ? 0xFF : (size == 2) ? 0xFFFF : 0xFFFFFFFF;
}

static void shift_store_dn(int reg, uint32_t val, int size)
{
    if (size == 1)
        cpu.d[reg] = (cpu.d[reg] & 0xFFFFFF00) | (val & 0xFF);
    else if (size == 2)
        cpu.d[reg] = (cpu.d[reg] & 0xFFFF0000) | (val & 0xFFFF);
    else
        cpu.d[reg] = val;
}

/* ASL: left arithmetic. C,X = last bit out; V = sign change during shift. */
static int op_asl_reg(uint16_t op, int count, int size, uint32_t mask)
{
    int reg = shift_dest_reg(op);
    uint32_t val = cpu.d[reg] & mask;
    int nbits = size * 8;
    int sign_changed = 0;
    int orig_sign = (val & ((uint32_t)1 << (nbits - 1))) ? 1 : 0;

    if (count >= nbits) count = nbits;
    uint32_t result = (count > 0) ? (val << count) & mask : val;
    int last_out = (count > 0) ? (val >> (nbits - count)) & 1 : 0;

    if (count > 0) {
        int new_sign = (result & ((uint32_t)1 << (nbits - 1))) ? 1 : 0;
        if (orig_sign != new_sign) sign_changed = 1;
    }

    shift_store_dn(reg, result, size);
    set_nz_from_val(result, size);
    cpu.sr &= ~(SR_V | SR_C | SR_X);
    if (count > 0) {
        if (last_out) cpu.sr |= SR_C | SR_X;
        if (sign_changed) cpu.sr |= SR_V;
    }
    return shift_cycles_register(size, count, 0);
}

/* ASR: right arithmetic. Sign-extend; C,X = last bit out; V = sign change. */
static int op_asr_reg(uint16_t op, int count, int size, uint32_t mask)
{
    int reg = shift_dest_reg(op);
    uint32_t val = cpu.d[reg] & mask;
    int32_t sval = (size == 1) ? (int32_t)(int8_t)val : (size == 2) ? (int32_t)(int16_t)val : (int32_t)val;
    int nbits = size * 8;
    int sign_changed = 0;
    int orig_sign = (sval < 0) ? 1 : 0;

    if (count >= nbits) count = nbits;
    int last_out = (count > 0) ? (val >> (count - 1)) & 1 : 0;
    int32_t result_s = (count > 0) ? (sval >> count) : sval;
    uint32_t result = (uint32_t)result_s & mask;

    if (count > 0) {
        int new_sign = (result_s < 0) ? 1 : 0;
        if (orig_sign != new_sign) sign_changed = 1;
    }

    shift_store_dn(reg, result, size);
    set_nz_from_val(result, size);
    cpu.sr &= ~(SR_V | SR_C | SR_X);
    if (count > 0) {
        if (last_out) cpu.sr |= SR_C | SR_X;
        if (sign_changed) cpu.sr |= SR_V;
    }
    return shift_cycles_register(size, count, 0);
}

/* LSL: left logical. C,X = last bit out; V always 0. */
static int op_lsl_reg(uint16_t op, int count, int size, uint32_t mask)
{
    int reg = shift_dest_reg(op);
    uint32_t val = cpu.d[reg] & mask;
    uint32_t result = val;
    int last_out = 0;

    if (count > 0) {
        result = (val << count) & mask;
        last_out = (val >> (size * 8 - count)) & 1;
    }

    shift_store_dn(reg, result, size);
    set_nz_from_val(result, size);
    cpu.sr &= ~(SR_V | SR_C | SR_X);
    if (count > 0 && last_out) cpu.sr |= SR_C | SR_X;
    return shift_cycles_register(size, count, 0);
}

/* LSR: right logical. C,X = last bit out; V always 0. */
static int op_lsr_reg(uint16_t op, int count, int size, uint32_t mask)
{
    int reg = shift_dest_reg(op);
    uint32_t val = cpu.d[reg] & mask;
    uint32_t result = val;
    int last_out = 0;

    if (count > 0) {
        result = (val >> count) & mask;
        last_out = (val >> (count - 1)) & 1;
    }

    shift_store_dn(reg, result, size);
    set_nz_from_val(result, size);
    cpu.sr &= ~(SR_V | SR_C | SR_X);
    if (count > 0 && last_out) cpu.sr |= SR_C | SR_X;
    return shift_cycles_register(size, count, 0);
}

/* ROL: rotate left without X. X unaffected; C = last bit out. */
static int op_rol_reg(uint16_t op, int count, int size, uint32_t mask)
{
    int reg = shift_dest_reg(op);
    uint32_t val = cpu.d[reg] & mask;
    uint32_t result = val;
    int last_out = 0;
    int nbits = size * 8;

    if (count > 0) {
        count %= nbits;
        if (count > 0) {
            result = ((val << count) | (val >> (nbits - count))) & mask;
            last_out = (val >> (nbits - count)) & 1;
        }
    }

    shift_store_dn(reg, result, size);
    set_nz_from_val(result, size);
    cpu.sr &= ~(SR_V | SR_C);
    if (count > 0 && last_out) cpu.sr |= SR_C;
    return shift_cycles_register(size, count, 0);
}

/* ROR: rotate right without X. X unaffected; C = last bit out. */
static int op_ror_reg(uint16_t op, int count, int size, uint32_t mask)
{
    int reg = shift_dest_reg(op);
    uint32_t val = cpu.d[reg] & mask;
    uint32_t result = val;
    int last_out = 0;
    int nbits = size * 8;

    if (count > 0) {
        count %= nbits;
        if (count > 0) {
            result = ((val >> count) | (val << (nbits - count))) & mask;
            last_out = (val >> (count - 1)) & 1;
        }
    }

    shift_store_dn(reg, result, size);
    set_nz_from_val(result, size);
    cpu.sr &= ~(SR_V | SR_C);
    if (count > 0 && last_out) cpu.sr |= SR_C;
    return shift_cycles_register(size, count, 0);
}

/* ROXL: rotate left with X. X = C = last bit out. */
static int op_roxl_reg(uint16_t op, int count, int size, uint32_t mask)
{
    int reg = shift_dest_reg(op);
    uint32_t val = cpu.d[reg] & mask;
    uint32_t xbit = (cpu.sr & SR_X) ? 1 : 0;
    uint32_t result = val;
    int last_out = 0;
    int nbits = size * 8;

    if (count > 0) {
        for (int i = 0; i < count; i++) {
            last_out = (val >> (nbits - 1)) & 1;
            val = ((val << 1) | xbit) & mask;
            xbit = last_out;
        }
        result = val;
    }

    shift_store_dn(reg, result, size);
    set_nz_from_val(result, size);
    cpu.sr &= ~(SR_V | SR_C | SR_X);
    if (count > 0 && last_out) cpu.sr |= SR_C | SR_X;
    return shift_cycles_register(size, count, 0);
}

/* ROXR: rotate right with X. X = C = last bit out. */
static int op_roxr_reg(uint16_t op, int count, int size, uint32_t mask)
{
    int reg = shift_dest_reg(op);
    uint32_t val = cpu.d[reg] & mask;
    uint32_t xbit = (cpu.sr & SR_X) ? 1 : 0;
    uint32_t result = val;
    int last_out = 0;
    int nbits = size * 8;

    if (count > 0) {
        for (int i = 0; i < count; i++) {
            last_out = val & 1;
            val = (val >> 1) | (xbit << (nbits - 1));
            xbit = last_out;
        }
        result = val & mask;
    }

    shift_store_dn(reg, result, size);
    set_nz_from_val(result, size);
    cpu.sr &= ~(SR_V | SR_C | SR_X);
    if (count > 0 && last_out) cpu.sr |= SR_C | SR_X;
    return shift_cycles_register(size, count, 0);
}

/* Memory shift: word only, count=1. EA in bits 5-0. */
static int op_shift_memory(uint16_t op)
{
    int ea_mode = ea_mode_from_op(op);
    int ea_reg = ea_reg_from_op(op);

    /* Reject Dn (0), An (1), #imm (7,4), d(PC) (7,2), (d8,PC,Xn) (7,3) */
    if (ea_mode == 0 || ea_mode == 1)
        return op_unimplemented(op);
    if (ea_mode == 7 && (ea_reg == 2 || ea_reg == 3 || ea_reg == 4))
        return op_unimplemented(op);

    int opkind = (op >> 9) & 1;  /* 0=ASL/ASR, 1=LSL/LSR */
    int dir = (op >> 8) & 1;     /* 0=right, 1=left */
    uint32_t val = ea_fetch_value(ea_mode, ea_reg, 2) & 0xFFFF;
    uint32_t result;
    int last_out;

    if (dir) {
        /* Left */
        last_out = (val >> 15) & 1;
        result = (val << 1) & 0xFFFF;
    } else {
        /* Right */
        last_out = val & 1;
        if (opkind == 0) {
            /* ASR: sign-extend */
            int16_t s = (int16_t)val;
            result = (uint32_t)(int32_t)(s >> 1) & 0xFFFF;
        } else {
            /* LSR */
            result = (val >> 1) & 0xFFFF;
        }
    }

    ea_store_value(ea_mode, ea_reg, 2, result);
    set_nz_from_val(result, 2);
    cpu.sr &= ~(SR_V | SR_C | SR_X);
    if (last_out) cpu.sr |= SR_C | SR_X;
    if (opkind == 0 && dir) {
        /* ASL: V = sign change */
        int orig_sign = (val & 0x8000) ? 1 : 0;
        int new_sign = (result & 0x8000) ? 1 : 0;
        if (orig_sign != new_sign) cpu.sr |= SR_V;
    }
    return shift_cycles_memory(ea_mode, ea_reg);
}

/* Memory rotate: ROL, ROR, ROXL, ROXR. Word only, count=1. Bits 11-8: 0100=ROL, 0101=ROR, 0110=ROXL, 0111=ROXR. */
static int op_shift_memory_rotate(uint16_t op)
{
    int ea_mode = ea_mode_from_op(op);
    int ea_reg = ea_reg_from_op(op);

    if (ea_mode == 0 || ea_mode == 1)
        return op_unimplemented(op);
    if (ea_mode == 7 && (ea_reg == 2 || ea_reg == 3 || ea_reg == 4))
        return op_unimplemented(op);

    int rot_type = (op >> 8) & 3;  /* 0=ROL, 1=ROR, 2=ROXL, 3=ROXR */
    int dir = (op >> 8) & 1;       /* 0=left (ROL/ROXL), 1=right (ROR/ROXR) */
    uint32_t val = ea_fetch_value(ea_mode, ea_reg, 2) & 0xFFFF;
    uint32_t result;
    int last_out;
    uint32_t xbit = (cpu.sr & SR_X) ? 1 : 0;

    if (dir == 0) {
        /* Left: ROL or ROXL */
        last_out = (val >> 15) & 1;
        if (rot_type == 2)  /* ROXL */
            result = ((val << 1) | xbit) & 0xFFFF;
        else                /* ROL */
            result = ((val << 1) | (val >> 15)) & 0xFFFF;
    } else {
        /* Right: ROR or ROXR */
        last_out = val & 1;
        if (rot_type == 3)  /* ROXR */
            result = ((val >> 1) | (xbit << 15)) & 0xFFFF;
        else                /* ROR */
            result = ((val >> 1) | (val << 15)) & 0xFFFF;
    }

    ea_store_value(ea_mode, ea_reg, 2, result);
    set_nz_from_val(result, 2);
    cpu.sr &= ~(SR_V | SR_C | SR_X);
    if (last_out) cpu.sr |= SR_C | SR_X;
    return shift_cycles_memory(ea_mode, ea_reg);
}

/* Register shift/rotate dispatcher */
static int dispatch_shift(uint16_t op)
{
    if (is_memory_shift(op)) {
        /* Memory: bits 7-6 = 11. Bits 11-8: 0000/0001=ASL/ASR, 0010/0011=LSL/LSR, 0100-0111=ROL/ROR/ROXL/ROXR. */
        if (((op >> 8) & 0x0C) == 0)
            return op_shift_memory(op);
        return op_shift_memory_rotate(op);
    }

    /* Register format */
    int size = shift_size(op);
    int count;
    int is_reg_count = (op >> 5) & 1;
    if (is_reg_count)
        count = shift_count_reg(op);
    else
        count = shift_count_imm(op);

    uint32_t mask = shift_mask(size);
    int op_type = shift_op_type(op);
    int dir = shift_direction(op);

    if (count == 0) {
        /* Count 0: no operation, but still take cycles. Flags unchanged for register count 0. */
        return shift_cycles_register(size, 0, is_reg_count);
    }

    switch (op_type) {
    case 0: return dir ? op_asl_reg(op, count, size, mask) : op_asr_reg(op, count, size, mask);
    case 1: return dir ? op_lsl_reg(op, count, size, mask) : op_lsr_reg(op, count, size, mask);
    case 2: return dir ? op_rol_reg(op, count, size, mask) : op_ror_reg(op, count, size, mask);
    case 3: return dir ? op_roxl_reg(op, count, size, mask) : op_roxr_reg(op, count, size, mask);
    case 4: return dir ? op_rol_reg(op, count, size, mask) : op_ror_reg(op, count, size, mask);
    case 5: return dir ? op_rol_reg(op, count, size, mask) : op_ror_reg(op, count, size, mask);
    case 6: return dir ? op_roxl_reg(op, count, size, mask) : op_roxr_reg(op, count, size, mask);
    case 7: return dir ? op_roxl_reg(op, count, size, mask) : op_roxr_reg(op, count, size, mask);
    default: return op_unimplemented(op);
    }
}

/* 0xExxx: route to shift or ADD. Second nibble 0-7 -> shift (ASL/ASR/LSL/LSR/ROL/ROR/ROXL/ROXR). */
int dispatch_Exxx(uint16_t op)
{
    int second = (op >> 8) & 0x0F;
    if (second >= 0 && second <= 7) return dispatch_shift(op);
    return dispatch_add(op);
}

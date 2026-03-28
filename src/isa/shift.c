/*
 * 68000 shift and rotate: ASL, ASR, LSL, LSR, ROL, ROR, ROXL, ROXR.
 * Register format: 1110 Ctt Ss dr i/r 0 oo rrr
 * Memory format: 1110 000o dr 11 MODE REG (word only, count=1)
 */

#include "cpu_internal.h"
#include "ea.h"
#include "bit.h"
#include "alu.h"
#include "memory.h"
#include "timing.h"

/* Register format: 1110 Ctt Ss dr i/r 0 oo rrr. Size in bits 7-6 (00=byte, 01=word, 10=long). */
static int shift_size(int op)
{
    int c = (op >> 6) & 3;
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
    return (op >> 8) & 1;  /* bit 8: 1=left, 0=right */
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



/* ASL: left arithmetic. C,X = last bit out; V = sign change at ANY point during shift. */
static int op_asl_reg(uint16_t op, int count, int size, uint32_t mask)
{
    int reg = shift_dest_reg(op);
    uint32_t val = cpu.d[reg] & mask;
    int nbits = size * 8;
    int orig_sign = (val >> (nbits - 1)) & 1;
    uint32_t result;
    int last_out = 0;
    int sign_changed = 0;

    if (count == 0) {
        result = val;
    } else if (count >= nbits) {
        /* All original bits shifted out */
        result = 0;
        last_out = (count == nbits) ? (val & 1) : 0;
        sign_changed = (val != 0);
    } else {
        /* 0 < count < nbits: V = top (count+1) bits not all equal to orig_sign */
        result = (val << count) & mask;
        last_out = (val >> (nbits - count)) & 1;
        uint32_t top_mask = (count >= nbits - 1) ? mask : (mask & ~(mask >> (count + 1)));
        uint32_t expected = orig_sign ? top_mask : 0;
        sign_changed = ((val & top_mask) != expected);
    }

    store_dn(reg, result, size);
    set_nz_from_val(result, size);  /* N,Z from result; V,C cleared; X unchanged */
    if (count > 0) {
        cpu.sr &= ~(SR_V | SR_C | SR_X);
        if (last_out) cpu.sr |= SR_C | SR_X;
        if (sign_changed) cpu.sr |= SR_V;
    }
    return shift_cycles_register(size, count, 0);
}

/* ASR: right arithmetic. Sign-extend; C,X = last bit out; V always 0. */
static int op_asr_reg(uint16_t op, int count, int size, uint32_t mask)
{
    int reg = shift_dest_reg(op);
    uint32_t val = cpu.d[reg] & mask;
    int32_t sval = sign_extend_sized(val, size);
    int nbits = size * 8;
    int orig_sign = (sval < 0) ? 1 : 0;
    uint32_t result;
    int last_out = 0;

    if (count == 0) {
        result = val;
    } else if (count > nbits) {
        /* Over-shifted: sign fill, C = X = 0 (no original bits remain) */
        result = orig_sign ? mask : 0;
        last_out = 0;
    } else if (count == nbits) {
        /* Last bit out is the MSB (sign bit) of original value */
        result = orig_sign ? mask : 0;
        last_out = orig_sign;
    } else {
        /* 0 < count < nbits: last bit out is bit[(count-1)] */
        last_out = (val >> (count - 1)) & 1;
        result = (uint32_t)(sval >> count) & mask;
    }

    store_dn(reg, result, size);
    set_nz_from_val(result, size);  /* N,Z from result; V,C cleared; X unchanged */
    if (count > 0) {
        cpu.sr &= ~(SR_V | SR_C | SR_X);
        if (last_out) cpu.sr |= SR_C | SR_X;
    }
    return shift_cycles_register(size, count, 0);
}

/* LSL: left logical. C,X = last bit out; V always 0. */
static int op_lsl_reg(uint16_t op, int count, int size, uint32_t mask)
{
    int reg = shift_dest_reg(op);
    uint32_t val = cpu.d[reg] & mask;
    int nbits = size * 8;
    uint32_t result = val;
    int last_out = 0;

    if (count > 0) {
        if (count > nbits) {
            result = 0;
            last_out = 0;
        } else if (count == nbits) {
            result = 0;
            last_out = val & 1;
        } else {
            result = (val << count) & mask;
            last_out = (val >> (nbits - count)) & 1;
        }
    }

    store_dn(reg, result, size);
    set_nz_from_val(result, size);  /* N,Z from result; V,C cleared; X unchanged */
    if (count > 0) {
        cpu.sr &= ~(SR_V | SR_C | SR_X);
        if (last_out) cpu.sr |= SR_C | SR_X;
    }
    return shift_cycles_register(size, count, 0);
}

/* LSR: right logical. C,X = last bit out; V always 0. */
static int op_lsr_reg(uint16_t op, int count, int size, uint32_t mask)
{
    int reg = shift_dest_reg(op);
    uint32_t val = cpu.d[reg] & mask;
    int nbits = size * 8;
    uint32_t result = val;
    int last_out = 0;

    if (count > 0) {
        if (count > nbits) {
            result = 0;
            last_out = 0;
        } else if (count == nbits) {
            result = 0;
            last_out = (val >> (nbits - 1)) & 1;
        } else {
            result = (val >> count) & mask;
            last_out = (val >> (count - 1)) & 1;
        }
    }

    store_dn(reg, result, size);
    set_nz_from_val(result, size);  /* N,Z from result; V,C cleared; X unchanged */
    if (count > 0) {
        cpu.sr &= ~(SR_V | SR_C | SR_X);
        if (last_out) cpu.sr |= SR_C | SR_X;
    }
    return shift_cycles_register(size, count, 0);
}

/* ROL: rotate left without X. X unaffected; C = last bit rotated through MSB. */
static int op_rol_reg(uint16_t op, int count, int size, uint32_t mask)
{
    int reg = shift_dest_reg(op);
    uint32_t val = cpu.d[reg] & mask;
    uint32_t result = val;
    int last_out = 0;
    int nbits = size * 8;

    if (count > 0) {
        /* C = bit that last exited MSB: original bit[(nbits - count%nbits) % nbits] */
        int bit_pos = (nbits - (count % nbits)) % nbits;
        last_out = (val >> bit_pos) & 1;
        int eff_count = count % nbits;
        if (eff_count > 0)
            result = ((val << eff_count) | (val >> (nbits - eff_count))) & mask;
        /* eff_count==0: full rotations, result == val */
    }

    store_dn(reg, result, size);
    set_nz_from_val(result, size);
    cpu.sr &= ~(SR_V | SR_C);
    if (last_out) cpu.sr |= SR_C;
    return shift_cycles_register(size, count, 0);
}

/* ROR: rotate right without X. X unaffected; C = last bit rotated through LSB. */
static int op_ror_reg(uint16_t op, int count, int size, uint32_t mask)
{
    int reg = shift_dest_reg(op);
    uint32_t val = cpu.d[reg] & mask;
    uint32_t result = val;
    int last_out = 0;
    int nbits = size * 8;

    if (count > 0) {
        /* C = bit that last exited LSB: original bit[(count-1) % nbits] */
        int bit_pos = (count - 1) % nbits;
        last_out = (val >> bit_pos) & 1;
        int eff_count = count % nbits;
        if (eff_count > 0)
            result = ((val >> eff_count) | (val << (nbits - eff_count))) & mask;
        /* eff_count==0: full rotations, result == val */
    }

    store_dn(reg, result, size);
    set_nz_from_val(result, size);
    cpu.sr &= ~(SR_V | SR_C);
    if (last_out) cpu.sr |= SR_C;
    return shift_cycles_register(size, count, 0);
}

/* ROXL: rotate left with X. X = C = last bit out; count=0: C = old X, X unchanged. */
static int op_roxl_reg(uint16_t op, int count, int size, uint32_t mask)
{
    int reg = shift_dest_reg(op);
    uint32_t val = cpu.d[reg] & mask;
    uint32_t xbit = (cpu.sr & SR_X) ? 1 : 0;  /* capture before any SR changes */
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

    store_dn(reg, result, size);
    set_nz_from_val(result, size);
    if (count > 0) {
        cpu.sr &= ~(SR_V | SR_C | SR_X);
        if (last_out) cpu.sr |= SR_C | SR_X;
    } else {
        /* count=0: C = old X, X unchanged, V cleared (set_nz_from_val already cleared V,C) */
        if (xbit) cpu.sr |= SR_C;
    }
    return shift_cycles_register(size, count, 0);
}

/* ROXR: rotate right with X. X = C = last bit out; count=0: C = old X, X unchanged. */
static int op_roxr_reg(uint16_t op, int count, int size, uint32_t mask)
{
    int reg = shift_dest_reg(op);
    uint32_t val = cpu.d[reg] & mask;
    uint32_t xbit = (cpu.sr & SR_X) ? 1 : 0;  /* capture before any SR changes */
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

    store_dn(reg, result, size);
    set_nz_from_val(result, size);
    if (count > 0) {
        cpu.sr &= ~(SR_V | SR_C | SR_X);
        if (last_out) cpu.sr |= SR_C | SR_X;
    } else {
        /* count=0: C = old X, X unchanged, V cleared (set_nz_from_val already cleared V,C) */
        if (xbit) cpu.sr |= SR_C;
    }
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
    ea_rmw_t rmw;
    uint32_t val = ea_read_rmw(ea_mode, ea_reg, 2, &rmw) & 0xFFFF;
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

    ea_write_rmw(&rmw, result);
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

/* Memory rotate: ROL, ROR, ROXL, ROXR. Word only, count=1.
 * Bits 11-8: 0100=ROXR, 0101=ROXL, 0110=ROR, 0111=ROL.
 * Bit 8: 1=left (ROXL/ROL), 0=right (ROXR/ROR).
 * Bit 9: 1=RO (ROL/ROR, X unaffected), 0=ROX (ROXL/ROXR, X updated). */
static int op_shift_memory_rotate(uint16_t op)
{
    int ea_mode = ea_mode_from_op(op);
    int ea_reg = ea_reg_from_op(op);

    if (ea_mode == 0 || ea_mode == 1)
        return op_unimplemented(op);
    if (ea_mode == 7 && (ea_reg == 2 || ea_reg == 3 || ea_reg == 4))
        return op_unimplemented(op);

    int nibble = (op >> 8) & 7;
    int dir_left = nibble & 1;    /* 1=left (ROXL/ROL), 0=right (ROXR/ROR) */
    int no_x = (nibble >> 1) & 1; /* 1=ROL/ROR (X unaffected), 0=ROXL/ROXR (X updated) */
    ea_rmw_t rmw;
    uint32_t val = ea_read_rmw(ea_mode, ea_reg, 2, &rmw) & 0xFFFF;
    uint32_t result;
    int last_out;
    uint32_t xbit = (cpu.sr & SR_X) ? 1 : 0;

    if (dir_left) {
        /* Left: ROL (7) or ROXL (5) */
        last_out = (val >> 15) & 1;
        if (no_x)  /* ROL */
            result = ((val << 1) | (val >> 15)) & 0xFFFF;
        else       /* ROXL */
            result = ((val << 1) | xbit) & 0xFFFF;
    } else {
        /* Right: ROR (6) or ROXR (4) */
        last_out = val & 1;
        if (no_x)  /* ROR */
            result = ((val >> 1) | (val << 15)) & 0xFFFF;
        else       /* ROXR */
            result = ((val >> 1) | (xbit << 15)) & 0xFFFF;
    }

    ea_write_rmw(&rmw, result);
    set_nz_from_val(result, 2);
    cpu.sr &= ~(SR_V | SR_C);
    if (last_out) cpu.sr |= SR_C;
    if (!no_x) {
        /* ROXL/ROXR: update X flag */
        cpu.sr &= ~SR_X;
        if (last_out) cpu.sr |= SR_X;
    }
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
    if (is_reg_count) {
        count = shift_count_reg(op);
        /* Raw count 0-63. Shift functions cap at nbits; ROL/ROR reduce mod nbits
         * internally; ROXL/ROXR use a loop. count=0 handled below (flags unchanged). */
    } else
        count = shift_count_imm(op);

    uint32_t mask = size_mask(size);
    int dir = shift_direction(op);  /* bit 8: 1=left, 0=right */
    int oo = (op >> 3) & 3;         /* bits 4-3: 00=AS, 01=LS, 10=ROX, 11=RO */

    /* count=0: each function handles flags (N/Z from val, V/C cleared, X unchanged).
     * For ROXL/ROXR: C = old X. Fall through for all. */

    switch (oo) {
    case 0: return dir ? op_asl_reg(op, count, size, mask) : op_asr_reg(op, count, size, mask);
    case 1: return dir ? op_lsl_reg(op, count, size, mask) : op_lsr_reg(op, count, size, mask);
    case 2: return dir ? op_roxl_reg(op, count, size, mask) : op_roxr_reg(op, count, size, mask);
    case 3: return dir ? op_rol_reg(op, count, size, mask) : op_ror_reg(op, count, size, mask);
    default: return op_unimplemented(op);
    }
}

/* 0xExxx: route to shift or ADD. 0xE is shift/rotate space; ADD uses 0xD. Route all 0xExxx to shift. */
int dispatch_Exxx(uint16_t op)
{
    /* BFxxx bitfield instructions occupy 0xE8C0-0xEFFF (68020+ only).
     * Bits 15-11 = 11101 and bits 7-6 = 11 → mask 0xF8C0, value 0xE8C0. */
    if ((op & 0xF8C0) == 0xE8C0 && cpu.features.has_full_ea)
        return op_bitfield(op);
    return dispatch_shift(op);
}

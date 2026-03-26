/*
 * BTST, BCHG, BCLR, BSET - bit manipulation.
 * Dn form: 0x01xx (bits 8-6: 000=BTST, 001=BCHG, 010=BCLR, 011=BSET). Bit # in Dn (bits 11-9).
 * #imm form: 0x08xx (same bits 8-6). 8-bit immediate follows. EA in bits 5-0.
 * Dn dest: long (bit mod 32). Memory: byte (bit mod 8).
 */

#include "cpu_internal.h"
#include "ea.h"
#include "memory.h"
#include "timing.h"

/* Get bit number: for Dn (long) mod 32, for memory (byte) mod 8. */
static int bit_number(int bit_reg, int ea_mode, int is_imm, uint8_t imm)
{
    int n = is_imm ? (imm & 0xFF) : (int)(cpu.d[bit_reg] & 0xFF);
    return (ea_mode == 0) ? (n & 31) : (n & 7);
}

/* Test bit: sets Z = 1 when bit is 0. */
static void set_z_from_bit(int bit_val)
{
    cpu.sr &= ~SR_Z;
    if (bit_val == 0)
        cpu.sr |= SR_Z;
}

/* Bit modify cycles (BCHG/BCLR/BSET). Dn: data-dependent on bit number.
 * bit >= 16: BCLR=10, BCHG/BSET=8. bit < 16: BCLR=8, BCHG/BSET=6. +4 if #imm.
 * Memory: 8 + (imm?4:0) + ea. */
static int bit_modify_cycles(int ea_mode, int ea_reg, int size, int is_imm, int opcode, int bit_n)
{
    if (ea_mode == 0) {
        int base = (opcode == 2) ? 10 : 8;
        if (bit_n < 16) base -= 2;
        return base + (is_imm ? 4 : 0);
    }
    return 8 + (is_imm ? 4 : 0) + ea_cycles(ea_mode, ea_reg, size);
}

/* BTST: test only. No store. Dn: 6 + (imm?4:0). #imm EA: 6 + ea. Memory: 4 + (imm?4:0) + ea. */
static int op_btst(int ea_mode, int ea_reg, int size, int bit_reg, int is_imm, uint8_t imm)
{
    int bit_n = bit_number(bit_reg, ea_mode, is_imm, imm);
    uint32_t val = ea_fetch_value(ea_mode, ea_reg, size) & size_mask(size);
    int bit_val = (int)((val >> bit_n) & 1);
    set_z_from_bit(bit_val);
    int base;
    if (ea_mode == 0 || (ea_mode == 7 && ea_reg == 4))
        base = 6;
    else
        base = 4;
    return base + (is_imm ? 4 : 0) + (ea_mode == 0 ? 0 : ea_cycles(ea_mode, ea_reg, size));
}

/* Modify ops (BCHG/BCLR/BSET): fetch, test, modify, store. */
typedef uint32_t (*bit_modify_fn)(uint32_t val, int bit_n);

static int op_bit_modify(int ea_mode, int ea_reg, int size, int bit_reg, int is_imm, uint8_t imm,
                        bit_modify_fn modify, int opcode)
{
    int bit_n = bit_number(bit_reg, ea_mode, is_imm, imm);
    ea_rmw_t rmw;
    uint32_t val = ea_read_rmw(ea_mode, ea_reg, size, &rmw) & size_mask(size);
    set_z_from_bit((int)((val >> bit_n) & 1));
    ea_write_rmw(&rmw, modify(val, bit_n));
    return bit_modify_cycles(ea_mode, ea_reg, size, is_imm, opcode, bit_n);
}

static uint32_t modify_bchg(uint32_t val, int bit_n) { return val ^ (1u << bit_n); }
static uint32_t modify_bclr(uint32_t val, int bit_n) { return val & ~(1u << bit_n); }
static uint32_t modify_bset(uint32_t val, int bit_n) { return val | (1u << bit_n); }

/* Shared decode and dispatch. Returns cycles or 0 (unimplemented). */
static int bit_execute(int ea_mode, int ea_reg, int bit_reg, int is_imm, uint8_t imm, int opcode)
{
    int size = (ea_mode == 0) ? 4 : 1;

    switch (opcode) {
    case 0: return op_btst(ea_mode, ea_reg, size, bit_reg, is_imm, imm);
    case 1: return op_bit_modify(ea_mode, ea_reg, size, bit_reg, is_imm, imm, modify_bchg, 1);
    case 2: return op_bit_modify(ea_mode, ea_reg, size, bit_reg, is_imm, imm, modify_bclr, 2);
    case 3: return op_bit_modify(ea_mode, ea_reg, size, bit_reg, is_imm, imm, modify_bset, 3);
    default: return 0;
    }
}

/* Dn form: 0x01xx. Bits 8-6 = opcode. Bits 11-9 = Dn (bit number). EA in 5-0. */
int op_bit_dn(uint16_t op)
{
    int ea_mode = ea_mode_from_op(op);
    int ea_reg = ea_reg_from_op(op);
    if (ea_mode == 1)
        return op_unimplemented(op);
    int cycles = bit_execute(ea_mode, ea_reg, (op >> 9) & 7, 0, 0, (op >> 6) & 3);
    return cycles ? cycles : op_unimplemented(op);
}

/* #imm form: 0x08xx-0x0Bxx. (op>>8)&3 = opcode. EA in 5-0. 8-bit immediate follows. */
int op_bit_imm(uint16_t op)
{
    int ea_mode = ea_mode_from_op(op);
    int ea_reg = ea_reg_from_op(op);
    if (ea_mode == 1)
        return op_unimplemented(op);
    uint8_t imm = (uint8_t)(fetch16() & 0xFF);
    int cycles = bit_execute(ea_mode, ea_reg, 0, 1, imm, (op >> 6) & 3);
    return cycles ? cycles : op_unimplemented(op);
}

/* ===========================================================================
 * 68020 BFxxx bitfield instructions. Opcodes 0xE8C0-0xEFFF.
 *
 * A "bitfield" is a contiguous run of bits in a data register or memory
 * location.  Rather than always operating on whole bytes, you can extract,
 * insert, or modify any 1-32 bit slice starting at any bit offset.
 *
 * The 8 variants are selected by bits 10-8 of the opcode:
 *   0 BFTST  — set N/Z from bitfield; no write-back
 *   1 BFEXTU — zero-extend bitfield into Dn
 *   2 BFCHG  — invert all bits in the bitfield
 *   3 BFEXTS — sign-extend bitfield into Dn
 *   4 BFCLR  — clear all bits in the bitfield
 *   5 BFFFO  — find first '1' bit; store its offset in Dn
 *   6 BFSET  — set all bits in the bitfield
 *   7 BFINS  — insert lowest 'width' bits of Dn into the bitfield
 *
 * Extension word (16 bits, immediately after opcode):
 *   bits 14-12  Dn   — data register used by BFEXTU/BFEXTS/BFINS/BFFFO
 *   bit  11     DO   — 0: offset is 5-bit signed immediate in bits 10-6
 *                      1: offset is the full 32-bit value of the register
 *                         whose number is in bits 10-8
 *   bits 10-6   offset field (5 bits)
 *   bit  5      DW   — 0: width is 5-bit immediate in bits 4-0 (0 means 32)
 *                      1: width is the lower 5 bits of the register whose
 *                         number is in bits 4-2 (0 means 32)
 *   bits  4-0   width field (5 bits)
 *
 * Bit numbering is MSB-first: offset 0 is the MSB of the first byte (or
 * bit 31 of a register), offset 1 is the next-most-significant bit, etc.
 * ===========================================================================
 */

/* Mask for a field of `width` bits (1-32). */
static uint32_t bf_mask(int width)
{
    return (width == 32) ? 0xFFFFFFFF : (1u << width) - 1;
}

/* Decode the bitfield extension word into Dn register number, offset, and width. */
static void bf_decode_ext(uint16_t ext, int *dn, int *offset, int *width)
{
    *dn = (ext >> 12) & 7;

    /* Offset: if DO=1, take the full 32-bit value of the data register whose
     * number is in bits 10-8 of the extension word.  If DO=0, the offset is
     * a 5-bit signed immediate in bits 10-6 (-16 to +15). */
    if ((ext >> 11) & 1) {
        *offset = (int)(int32_t)cpu.d[(ext >> 8) & 7];
    } else {
        int raw5 = (ext >> 6) & 0x1F;
        *offset  = raw5 >= 16 ? raw5 - 32 : raw5;  /* sign-extend 5-bit */
    }

    /* Width: if DW=1, take the lower 5 bits of the register in bits 4-2.
     * If DW=0, the width is a 5-bit immediate in bits 4-0 (0 means 32). */
    if ((ext >> 5) & 1)
        *width = (int)(cpu.d[(ext >> 2) & 7] & 31);
    else
        *width = ext & 31;
    if (*width == 0) *width = 32;
}

/* ---------------------------------------------------------------------------
 * Register-direct bitfield helpers.
 *
 * The 68020 treats a data register as a 32-bit circular bit queue for
 * bitfield operations.  Offset 0 = MSB (bit 31 in standard notation),
 * offset 31 = LSB (bit 0).
 *
 * We handle the wrap-around case (eff_off + width > 32) by duplicating the
 * register into a 64-bit integer and extracting from there.
 * ---------------------------------------------------------------------------
 */

/* Read `width` bits starting at bit-offset `eff_off` from a data register. */
static uint32_t bf_reg_read(int reg, int eff_off, int width)
{
    /* Duplicate the register so wrapping is a simple shift on 64 bits. */
    uint64_t circular = ((uint64_t)cpu.d[reg] << 32) | cpu.d[reg];
    return (uint32_t)((circular >> (64 - eff_off - width)) & (uint64_t)bf_mask(width));
}

/* Write `width` bits starting at bit-offset `eff_off` into a data register. */
static void bf_reg_write(int reg, int eff_off, int width, uint32_t val)
{
    val &= bf_mask(width);
    if (eff_off + width <= 32) {
        /* Field fits without wrapping: simple shift and mask. */
        int shift    = 32 - eff_off - width;
        uint32_t m   = bf_mask(width) << shift;
        cpu.d[reg]   = (cpu.d[reg] & ~m) | (val << shift);
    } else {
        /* Field wraps: write hi_w bits to the lower part of the register,
         * and lo_w bits to the upper part. */
        int hi_w     = 32 - eff_off;
        int lo_w     = width - hi_w;
        uint32_t hi  = val >> lo_w;                          /* top hi_w bits of value */
        uint32_t lo  = val & bf_mask(lo_w);                  /* bottom lo_w bits */
        /* hi goes to register bits (hi_w-1)..0 */
        cpu.d[reg]   = (cpu.d[reg] & ~bf_mask(hi_w)) | hi;
        /* lo goes to register bits 31..(32-lo_w) */
        uint32_t lm  = bf_mask(lo_w) << (32 - lo_w);
        cpu.d[reg]   = (cpu.d[reg] & ~lm) | (lo << (32 - lo_w));
    }
}

/* ---------------------------------------------------------------------------
 * Memory bitfield helpers.
 *
 * The offset is in bits relative to the base address.  Positive offsets go
 * right; negative offsets go left (before the base byte).
 *
 * We compute:
 *   byte_off  = floor(offset / 8)   — byte index relative to base
 *   bit_off   = offset - byte_off*8 — bit position within that byte (0=MSB)
 *
 * Then we read just enough bytes to cover the entire field, pack them into
 * a uint64_t, and extract or replace the right bits.
 * ---------------------------------------------------------------------------
 */

/* Decompose a bit offset into (byte index, bit-within-byte 0=MSB). */
static void bf_mem_addr(int offset, int *byte_off, int *bit_off)
{
    /* Arithmetic right shift gives floor(offset/8) on two's-complement machines. */
    *byte_off = offset >> 3;
    *bit_off  = offset - (*byte_off * 8);   /* always 0-7 */
}

/* Read `width` bits of the bitfield at (base, offset) from memory. */
static uint32_t bf_mem_read(uint32_t base, int offset, int width)
{
    int byte_off, bit_off;
    bf_mem_addr(offset, &byte_off, &bit_off);

    /* How many bytes do we need?  At most 5 (width=32, bit_off=7 → 39 bits → 5 bytes). */
    int nbytes = (bit_off + width + 7) >> 3;
    uint64_t buf = 0;
    for (int i = 0; i < nbytes; i++)
        buf = (buf << 8) | mem_read8(base + byte_off + i);

    /* The field starts `bit_off` bits into the most-significant byte of buf.
     * Shift right so the LSB of the field aligns with bit 0. */
    int rshift = nbytes * 8 - bit_off - width;
    return (uint32_t)((buf >> rshift) & (uint64_t)bf_mask(width));
}

/* Write `width` bits of `val` into the bitfield at (base, offset) in memory. */
static void bf_mem_write(uint32_t base, int offset, int width, uint32_t val)
{
    int byte_off, bit_off;
    bf_mem_addr(offset, &byte_off, &bit_off);

    int nbytes     = (bit_off + width + 7) >> 3;
    int total_bits = nbytes * 8;
    int lshift     = total_bits - bit_off - width;  /* align field left in the window */

    uint64_t mask = (uint64_t)bf_mask(width) << lshift;
    uint64_t newv = (uint64_t)(val & bf_mask(width)) << lshift;

    for (int i = 0; i < nbytes; i++) {
        int     shift    = (nbytes - 1 - i) * 8;
        uint8_t old_byte = mem_read8(base + byte_off + i);
        uint8_t m        = (uint8_t)(mask >> shift);
        uint8_t v        = (uint8_t)(newv >> shift);
        mem_write8(base + byte_off + i, (old_byte & ~m) | v);
    }
}

/* ---------------------------------------------------------------------------
 * Main bitfield dispatcher: op_bitfield()
 * Called from dispatch_Exxx when (op & 0xF8C0) == 0xE8C0.
 * ---------------------------------------------------------------------------
 */
int op_bitfield(uint16_t op)
{
    int bf_op   = (op >> 8) & 7;  /* 0-7: selects BFTST/BFEXTU/BFCHG/BFEXTS/BFCLR/BFFFO/BFSET/BFINS */
    int ea_mode = (op >> 3) & 7;
    int ea_reg  =  op       & 7;

    /* (An)+  and -(An) are invalid EA modes for all bitfield instructions. */
    if (ea_mode == 1 || ea_mode == 3 || ea_mode == 4)
        return op_unimplemented(op);

    uint16_t ext = fetch16();
    int dn, offset, width;
    bf_decode_ext(ext, &dn, &offset, &width);

    /* --- Read the current bitfield value ---------------------------------- */
    uint32_t field;
    uint32_t mem_base = 0;
    int eff_off;     /* effective offset: mod-32 for register, raw for memory */

    if (ea_mode == 0) {
        eff_off = offset & 31;  /* register: offset wraps modulo 32 */
        field   = bf_reg_read(ea_reg, eff_off, width);
    } else {
        if (!ea_address_no_fetch(ea_mode, ea_reg, &mem_base))
            return op_unimplemented(op);
        eff_off = offset;
        field   = bf_mem_read(mem_base, offset, width);
    }

    /* --- Set N/Z from the bitfield (default; BFINS overrides below) ------- */
    cpu.sr &= ~(SR_N | SR_Z | SR_V | SR_C);
    if (field >> (width - 1)) cpu.sr |= SR_N;  /* MSB of the bitfield */
    if (field == 0)            cpu.sr |= SR_Z;

    /* --- Compute the result and write back where needed ------------------- */
    uint32_t result = 0;

    switch (bf_op) {
    case 0: /* BFTST — test only; flags already set above */
        return bf_cycles(bf_op, ea_mode, ea_reg);

    case 1: /* BFEXTU — zero-extend field into Dn; flags from original field */
        cpu.d[dn] = field;
        return bf_cycles(bf_op, ea_mode, ea_reg);

    case 2: /* BFCHG — flip every bit in the field */
        result = field ^ bf_mask(width);
        break;

    case 3: /* BFEXTS — sign-extend field into Dn; flags from original field */
        /* If the MSB of the bitfield is 1, fill the upper bits with 1s. */
        cpu.d[dn] = (field & (1u << (width - 1)))
                    ? (field | ~bf_mask(width))
                    : field;
        return bf_cycles(bf_op, ea_mode, ea_reg);

    case 4: /* BFCLR — clear every bit to 0 */
        result = 0;
        break;

    case 5: /* BFFFO — find first '1' from MSB; store absolute offset in Dn */
        {
            int pos = width;   /* default: all zeros, return offset+width */
            for (int i = 0; i < width; i++) {
                /* Bit i from MSB of the bitfield is at bit position (width-1-i). */
                if ((field >> (width - 1 - i)) & 1) { pos = i; break; }
            }
            cpu.d[dn] = (uint32_t)(eff_off + pos);
        }
        return bf_cycles(bf_op, ea_mode, ea_reg);

    case 6: /* BFSET — set every bit to 1 */
        result = bf_mask(width);
        break;

    case 7: /* BFINS — insert lower `width` bits of Dn into field */
        result = cpu.d[dn] & bf_mask(width);
        /* Override: flags come from the value being *inserted*, not the old field. */
        cpu.sr &= ~(SR_N | SR_Z);
        if (result >> (width - 1)) cpu.sr |= SR_N;
        if (result == 0)            cpu.sr |= SR_Z;
        break;

    default: return op_unimplemented(op);
    }

    /* Write back for BFCHG / BFCLR / BFSET / BFINS */
    if (ea_mode == 0)
        bf_reg_write(ea_reg, eff_off, width, result);
    else
        bf_mem_write(mem_base, offset, width, result);

    return bf_cycles(bf_op, ea_mode, ea_reg);
}

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

/* Base cycles for modify ops (BCHG/BCLR/BSET): 8 + (imm ? 4 : 0) + memory EA cycles. */
static int bit_modify_cycles(int ea_mode, int ea_reg, int size, int is_imm)
{
    return 8 + (is_imm ? 4 : 0) + (ea_mode == 0 ? 0 : ea_cycles(ea_mode, ea_reg, size) * 2);
}

/* BTST: test only. No store. */
static int op_btst(int ea_mode, int ea_reg, int size, int bit_reg, int is_imm, uint8_t imm)
{
    int bit_n = bit_number(bit_reg, ea_mode, is_imm, imm);
    uint32_t val = ea_fetch_value(ea_mode, ea_reg, size) & size_mask(size);
    int bit_val = (int)((val >> bit_n) & 1);
    set_z_from_bit(bit_val);
    return 4 + (is_imm ? 4 : 0) + (ea_mode == 0 ? 0 : ea_cycles(ea_mode, ea_reg, size));
}

/* Modify ops (BCHG/BCLR/BSET): fetch, test, modify, store. */
typedef uint32_t (*bit_modify_fn)(uint32_t val, int bit_n);

static int op_bit_modify(int ea_mode, int ea_reg, int size, int bit_reg, int is_imm, uint8_t imm,
                        bit_modify_fn modify)
{
    int bit_n = bit_number(bit_reg, ea_mode, is_imm, imm);
    uint32_t val = ea_fetch_value(ea_mode, ea_reg, size) & size_mask(size);
    set_z_from_bit((int)((val >> bit_n) & 1));
    ea_store_value(ea_mode, ea_reg, size, modify(val, bit_n));
    return bit_modify_cycles(ea_mode, ea_reg, size, is_imm);
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
    case 1: return op_bit_modify(ea_mode, ea_reg, size, bit_reg, is_imm, imm, modify_bchg);
    case 2: return op_bit_modify(ea_mode, ea_reg, size, bit_reg, is_imm, imm, modify_bclr);
    case 3: return op_bit_modify(ea_mode, ea_reg, size, bit_reg, is_imm, imm, modify_bset);
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
    int cycles = bit_execute(ea_mode, ea_reg, 0, 1, imm, (op >> 8) & 3);
    return cycles ? cycles : op_unimplemented(op);
}

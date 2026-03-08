#include "cpu_internal.h"
#include "ea.h"
#include "alu.h"
#include "memory.h"
#include "timing.h"

/*
 * ADD/SUB/CMP using EA module. Supports all sizes (B, W, L) and EA modes
 * implemented in ea.c: Dn, An, (An), (An)+, -(An), d(An), abs.w, abs.l, d(PC), #imm.
 *
 * Encoding: bits 7-6=size, bit 9=direction, bits 11-9=Dn, bits 5-0=EA mode/reg.
 *
 * ADDX/SUBX: Format 1101/1001 Rx 1 SIZE 0 0 R/M Ry. R/M=0: Dy,Dx. R/M=1: -(Ay),-(Ax).
 * Distinguish from ADD/SUB: (op & 0x130) == 0x100 (bit 8=1, bits 5-4=00).
 */

static int alu_size(int op)
{
    int size_code = (op >> 6) & 3;
    return (size_code == 0) ? 1 : (size_code == 1) ? 2 : 4;
}

/* Dn register: bits 11-9 encoded as (high_byte - base) >> 3. Base: ADD 0xD0/0xE0/0xF0, SUB 0x90, CMP 0xB0 */
static int alu_dn_reg(uint16_t op, uint8_t base)
{
    return ((op >> 8) - base) >> 3;
}

static uint32_t alu_size_mask(int size)
{
    return (size == 1) ? 0xFF : (size == 2) ? 0xFFFF : 0xFFFFFFFF;
}

static void alu_store_dn(int reg, uint32_t result, int size)
{
    if (size == 1)
        cpu.d[reg] = (cpu.d[reg] & 0xFFFFFF00) | (result & 0xFF);
    else if (size == 2)
        cpu.d[reg] = (cpu.d[reg] & 0xFFFF0000) | (result & 0xFFFF);
    else
        cpu.d[reg] = result;
}

static int alu_is_an_word_32bit(int ea_mode, int size)
{
    return ea_mode == 1 && size == 2;
}

/* 68000: byte ops cannot use An (Address Register Direct) - illegal instruction. Returns 1 if invalid. */
static int alu_reject_byte_an(uint16_t op, int ea_mode, int size)
{
    if (ea_mode == 1 && size == 1) {
        op_unimplemented(op);
        return 1;
    }
    return 0;
}

static int op_add_sub_generic(uint16_t op, int size, uint8_t base)
{
    int dn_reg = alu_dn_reg(op, base);
    int ea_mode = (op >> 3) & 7;
    int ea_reg = op & 7;
    int dir = (op >> 9) & 1;  /* 0 = <ea> to Dn, 1 = Dn to <ea> */
    int is_add = (base == 0xD0);

    if (alu_reject_byte_an(op, ea_mode, size))
        return 0;  /* unreachable - longjmps */

    uint32_t src, dest_val, result;
    uint32_t mask = alu_size_mask(size);
    int store_size = size;
    uint32_t src_for_flags;

    if (dir == 0) {
        /* ADD/SUB <ea>, Dn */
        src = ea_fetch_value(ea_mode, ea_reg, size);
        dest_val = cpu.d[dn_reg] & mask;
        result = is_add ? (dest_val + src) & mask : (dest_val - src) & mask;
        alu_store_dn(dn_reg, result, size);
        src_for_flags = src;
    } else {
        /* ADD/SUB Dn, <ea> */
        src = cpu.d[dn_reg];
        dest_val = ea_fetch_value(ea_mode, ea_reg, size);
        if (alu_is_an_word_32bit(ea_mode, size)) {
            int32_t src_se = (int32_t)(int16_t)(src & 0xFFFF);
            dest_val = cpu.a[ea_reg];
            result = is_add ? dest_val + src_se : dest_val - src_se;
            store_size = 4;
            src_for_flags = (uint32_t)(int32_t)(int16_t)(src & 0xFFFF);
        } else {
            src &= mask;
            dest_val &= mask;
            result = is_add ? (dest_val + src) & mask : (dest_val - src) & mask;
            src_for_flags = src;
        }
        ea_store_value(ea_mode, ea_reg, store_size, result);
    }

    if (is_add)
        set_nzvc_add_sized(result, dest_val, src_for_flags, store_size);
    else
        set_nzvc_sub_sized(result, dest_val, src_for_flags, store_size);
    return add_sub_cycles(ea_mode, ea_reg, size, dir);
}

/* CMP only has <ea>, Dn (no store) */
static int op_cmp_generic(uint16_t op, int size)
{
    int dn_reg = alu_dn_reg(op, 0xB0);
    int ea_mode = (op >> 3) & 7;
    int ea_reg = op & 7;

    if (alu_reject_byte_an(op, ea_mode, size))
        return 0;  /* unreachable - longjmps */

    uint32_t mask = alu_size_mask(size);
    uint32_t dest_val = cpu.d[dn_reg] & mask;
    uint32_t src = ea_fetch_value(ea_mode, ea_reg, size) & mask;
    uint32_t result = (dest_val - src) & mask;

    set_nzvc_sub_sized(result, dest_val, src, size);
    return cmp_cycles(ea_mode, ea_reg, size);
}

static uint32_t alu_mem_read_sized(uint32_t addr, int size)
{
    if (size == 1) return mem_read8(addr) & 0xFF;
    if (size == 2) return mem_read16(addr) & 0xFFFF;
    return mem_read32(addr);
}

static void alu_mem_write_sized(uint32_t addr, int size, uint32_t value)
{
    if (size == 1) mem_write8(addr, (uint8_t)(value & 0xFF));
    else if (size == 2) mem_write16(addr, (uint16_t)(value & 0xFFFF));
    else mem_write32(addr, value);
}

/* ADDA/SUBA/CMPA: opmode (bits 8-6) 011=word, 111=long. Destination An, source EA. */
static int alu_is_adda_suba_cmpa(uint16_t op)
{
    int opmode = (op >> 6) & 7;
    return opmode == 3 || opmode == 7;
}

static int op_adda(uint16_t op)
{
    int an_reg = (op >> 9) & 7;
    int ea_mode = (op >> 3) & 7;
    int ea_reg = op & 7;
    int size = ((op >> 6) & 7) == 7 ? 4 : 2;  /* 111=long, 011=word */

    uint32_t src = ea_fetch_value(ea_mode, ea_reg, size);
    uint32_t dest = cpu.a[an_reg];
    uint32_t result;

    if (size == 2)
        src = (uint32_t)(int32_t)(int16_t)(src & 0xFFFF);
    result = dest + src;
    cpu.a[an_reg] = result;
    return add_sub_cycles(ea_mode, ea_reg, size, 0);
}

static int op_suba(uint16_t op)
{
    int an_reg = (op >> 9) & 7;
    int ea_mode = (op >> 3) & 7;
    int ea_reg = op & 7;
    int size = ((op >> 6) & 7) == 7 ? 4 : 2;

    uint32_t src = ea_fetch_value(ea_mode, ea_reg, size);
    uint32_t dest = cpu.a[an_reg];
    uint32_t result;

    if (size == 2)
        src = (uint32_t)(int32_t)(int16_t)(src & 0xFFFF);
    result = dest - src;
    cpu.a[an_reg] = result;
    return add_sub_cycles(ea_mode, ea_reg, size, 0);
}

static int op_cmpa(uint16_t op)
{
    int an_reg = (op >> 9) & 7;
    int ea_mode = (op >> 3) & 7;
    int ea_reg = op & 7;
    int size = ((op >> 6) & 7) == 7 ? 4 : 2;

    uint32_t dest = cpu.a[an_reg];
    uint32_t src = ea_fetch_value(ea_mode, ea_reg, size);
    uint32_t result;

    if (size == 2)
        src = (uint32_t)(int32_t)(int16_t)(src & 0xFFFF);
    result = (dest - src) & 0xFFFFFFFF;
    set_nzvc_sub_sized(result, dest, src, 4);
    return cmp_cycles(ea_mode, ea_reg, size);
}

/* ADDX/SUBX: dest = dest op src op X. Format: 1101/1001 Rx 1 SIZE 0 0 R/M Ry. Z: cleared if nonzero. */
static int op_addx_subx(uint16_t op, int is_add)
{
    int dest_reg = (op >> 9) & 7;
    int src_reg = (op >> 0) & 7;
    int size = alu_size(op);
    int is_memory_mode = (op >> 3) & 1;
    uint32_t xbit = (cpu.sr & SR_X) ? 1 : 0;
    uint32_t mask = alu_size_mask(size);

    if (is_memory_mode == 0) {
        /* ADDX/SUBX Dy, Dx (register mode) */
        uint32_t src = cpu.d[src_reg] & mask;
        uint32_t dest_val = cpu.d[dest_reg] & mask;
        uint32_t result = is_add ? (dest_val + src + xbit) & mask : (dest_val - src - xbit) & mask;
        alu_store_dn(dest_reg, result, size);
        if (is_add)
            set_nzvc_addx_sized(result, dest_val, src, size);
        else
            set_nzvc_subx_sized(result, dest_val, src, size);
    } else {
        /* ADDX/SUBX -(Ay), -(Ax): decrement both, fetch, op, store */
        cpu.a[src_reg] -= ea_step(src_reg, size);
        cpu.a[dest_reg] -= ea_step(dest_reg, size);
        uint32_t addr_x = cpu.a[dest_reg];
        uint32_t addr_y = cpu.a[src_reg];
        uint32_t src = alu_mem_read_sized(addr_y, size);
        uint32_t dest_val = alu_mem_read_sized(addr_x, size);
        uint32_t result = is_add ? (dest_val + src + xbit) & mask : (dest_val - src - xbit) & mask;
        alu_mem_write_sized(addr_x, size, result);
        if (is_add)
            set_nzvc_addx_sized(result, dest_val, src, size);
        else
            set_nzvc_subx_sized(result, dest_val, src, size);
    }
    return addx_subx_cycles(is_memory_mode, size);
}

/* MOVEQ #imm, Dn: sign-extend 8-bit immediate to 32-bit, load into Dn. Sets N,Z; clears V,C. */
int op_moveq(uint16_t op)
{
    int dest_reg = (op >> 9) & 7;
    int32_t imm = (int8_t)(op & 0xFF);
    uint32_t result = (uint32_t)imm;
    cpu.d[dest_reg] = result;
    set_nz_from_val(result, 4);
    return CYCLES_MOVEQ;
}

/* 0x9xxx: SUB, SUBA, or SUBX. SUBA when opmode 011/111; SUBX when (op & 0x130)==0x100. */
int dispatch_9xxx(uint16_t op)
{
    if (alu_is_adda_suba_cmpa(op))
        return op_suba(op);
    if ((op & 0x130) == 0x100)
        return op_addx_subx(op, 0);
    return op_add_sub_generic(op, alu_size(op), 0x90);
}

/* 0xBxxx: CMP or CMPA. CMPA when opmode 011/111. */
int dispatch_Bxxx(uint16_t op)
{
    if (alu_is_adda_suba_cmpa(op))
        return op_cmpa(op);
    return op_cmp_generic(op, alu_size(op));
}

/* 0xDxxx/0xExxx/0xFxxx: ADD, ADDA, or ADDX. ADDA when opmode 011/111; ADDX when (op & 0x130)==0x100. */
int dispatch_add(uint16_t op)
{
    if (alu_is_adda_suba_cmpa(op))
        return op_adda(op);
    if ((op & 0x130) == 0x100)
        return op_addx_subx(op, 1);
    return op_add_sub_generic(op, alu_size(op), 0xD0);
}

#include "cpu_internal.h"
#include "ea.h"
#include "alu.h"
#include "memory.h"

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

/* 68000: byte ops cannot use An (Address Register Direct) - illegal instruction. Returns 1 if invalid. */
static int alu_reject_byte_an(uint16_t op, int ea_mode, int size)
{
    if (ea_mode == 1 && size == 1) {
        op_unimplemented(op);
        return 1;
    }
    return 0;
}

static void op_add_generic(uint16_t op, int size)
{
    /* ADD base 0xD0: 0xD0=D0, 0xD8=D1, 0xE0=D2, 0xE8=D3, 0xF0=D4, etc. */
    int dn_reg = alu_dn_reg(op, 0xD0);
    int ea_mode = (op >> 3) & 7;
    int ea_reg = op & 7;
    int dir = (op >> 9) & 1;  /* 0 = <ea> to Dn, 1 = Dn to <ea> */

    if (alu_reject_byte_an(op, ea_mode, size))
        return;

    if (dir == 0) {
        /* ADD <ea>, Dn */
        uint32_t src = ea_fetch_value(ea_mode, ea_reg, size);
        uint32_t dest_val = cpu.d[dn_reg];
        uint32_t result;
        if (size == 1) {
            dest_val &= 0xFF;
            result = (dest_val + src) & 0xFF;
            cpu.d[dn_reg] = (cpu.d[dn_reg] & 0xFFFFFF00) | result;
        } else if (size == 2) {
            dest_val &= 0xFFFF;
            result = (dest_val + src) & 0xFFFF;
            cpu.d[dn_reg] = (cpu.d[dn_reg] & 0xFFFF0000) | result;
        } else {
            result = dest_val + src;
            cpu.d[dn_reg] = result;
        }
        set_nzvc_add_sized(result, dest_val, src, size);
    } else {
        /* ADD Dn, <ea> */
        uint32_t src = cpu.d[dn_reg];
        uint32_t dest_val = ea_fetch_value(ea_mode, ea_reg, size);
        uint32_t result;
        if (size == 1) {
            src &= 0xFF;
            dest_val &= 0xFF;
            result = (dest_val + src) & 0xFF;
        } else if (size == 2) {
            src &= 0xFFFF;
            dest_val &= 0xFFFF;
            result = (dest_val + src) & 0xFFFF;
        } else {
            result = dest_val + src;
        }
        ea_store_value(ea_mode, ea_reg, size, result);
        set_nzvc_add_sized(result, dest_val, src, size);
    }
}

static void op_sub_generic(uint16_t op, int size)
{
    int dn_reg = alu_dn_reg(op, 0x90);
    int ea_mode = (op >> 3) & 7;
    int ea_reg = op & 7;
    int dir = (op >> 9) & 1;

    if (alu_reject_byte_an(op, ea_mode, size))
        return;

    if (dir == 0) {
        /* SUB <ea>, Dn */
        uint32_t src = ea_fetch_value(ea_mode, ea_reg, size);
        uint32_t dest_val = cpu.d[dn_reg];
        uint32_t result;
        if (size == 1) {
            dest_val &= 0xFF;
            result = (dest_val - src) & 0xFF;
            cpu.d[dn_reg] = (cpu.d[dn_reg] & 0xFFFFFF00) | result;
        } else if (size == 2) {
            dest_val &= 0xFFFF;
            result = (dest_val - src) & 0xFFFF;
            cpu.d[dn_reg] = (cpu.d[dn_reg] & 0xFFFF0000) | result;
        } else {
            result = dest_val - src;
            cpu.d[dn_reg] = result;
        }
        set_nzvc_sub_sized(result, dest_val, src, size);
    } else {
        /* SUB Dn, <ea> */
        uint32_t src = cpu.d[dn_reg];
        uint32_t dest_val = ea_fetch_value(ea_mode, ea_reg, size);
        uint32_t result;
        if (size == 1) {
            src &= 0xFF;
            dest_val &= 0xFF;
            result = (dest_val - src) & 0xFF;
        } else if (size == 2) {
            src &= 0xFFFF;
            dest_val &= 0xFFFF;
            result = (dest_val - src) & 0xFFFF;
        } else {
            result = dest_val - src;
        }
        ea_store_value(ea_mode, ea_reg, size, result);
        set_nzvc_sub_sized(result, dest_val, src, size);
    }
}

/* CMP only has <ea>, Dn (no store) */
static void op_cmp_generic(uint16_t op, int size)
{
    int dn_reg = alu_dn_reg(op, 0xB0);
    int ea_mode = (op >> 3) & 7;
    int ea_reg = op & 7;

    if (alu_reject_byte_an(op, ea_mode, size))
        return;

    uint32_t dest_val = cpu.d[dn_reg];
    uint32_t src = ea_fetch_value(ea_mode, ea_reg, size);
    uint32_t result;

    if (size == 1) {
        dest_val &= 0xFF;
        src &= 0xFF;
        result = (dest_val - src) & 0xFF;
    } else if (size == 2) {
        dest_val &= 0xFFFF;
        src &= 0xFFFF;
        result = (dest_val - src) & 0xFFFF;
    } else {
        result = dest_val - src;
    }
    set_nzvc_sub_sized(result, dest_val, src, size);
}

/* ADDX: dest = dest + src + X. Format: 1101 Rx 1 SIZE 0 0 R/M Ry. Z: cleared if nonzero. */
static void op_addx(uint16_t op)
{
    int dest_reg = (op >> 9) & 7;
    int src_reg = (op >> 0) & 7;
    int size = alu_size(op);
    int is_memory_mode = (op >> 3) & 1;
    uint32_t xbit = (cpu.sr & SR_X) ? 1 : 0;

    if (is_memory_mode == 0) {
        /* ADDX Dy, Dx */
        uint32_t src = cpu.d[src_reg];
        uint32_t dest_val = cpu.d[dest_reg];
        uint32_t result;
        if (size == 1) {
            src &= 0xFF;
            dest_val &= 0xFF;
            result = (dest_val + src + xbit) & 0xFF;
            cpu.d[dest_reg] = (cpu.d[dest_reg] & 0xFFFFFF00) | result;
        } else if (size == 2) {
            src &= 0xFFFF;
            dest_val &= 0xFFFF;
            result = (dest_val + src + xbit) & 0xFFFF;
            cpu.d[dest_reg] = (cpu.d[dest_reg] & 0xFFFF0000) | result;
        } else {
            result = dest_val + src + xbit;
            cpu.d[dest_reg] = result;
        }
        set_nzvc_addx_sized(result, dest_val, src, size);
    } else {
        /* ADDX -(Ay), -(Ax): decrement both, fetch, add+X, store to Ax */
        cpu.a[src_reg] -= ea_step(src_reg, size);
        cpu.a[dest_reg] -= ea_step(dest_reg, size);
        uint32_t addr_y = cpu.a[src_reg], addr_x = cpu.a[dest_reg];
        uint32_t src, dest_val;
        if (size == 1) {
            src = mem_read8(addr_y) & 0xFF;
            dest_val = mem_read8(addr_x) & 0xFF;
            uint32_t result = (dest_val + src + xbit) & 0xFF;
            mem_write8(addr_x, (uint8_t)result);
            set_nzvc_addx_sized(result, dest_val, src, size);
        } else if (size == 2) {
            src = mem_read16(addr_y) & 0xFFFF;
            dest_val = mem_read16(addr_x) & 0xFFFF;
            uint32_t result = (dest_val + src + xbit) & 0xFFFF;
            mem_write16(addr_x, (uint16_t)result);
            set_nzvc_addx_sized(result, dest_val, src, size);
        } else {
            src = mem_read32(addr_y);
            dest_val = mem_read32(addr_x);
            uint32_t result = dest_val + src + xbit;
            mem_write32(addr_x, result);
            set_nzvc_addx_sized(result, dest_val, src, size);
        }
    }
}

/* SUBX: dest = dest - src - X. Format: 1001 Dy 1 SIZE 0 0 R/M Dx. Z: cleared if nonzero. */
static void op_subx(uint16_t op)
{
    int dest_reg = (op >> 9) & 7;   /* dest Dy/Ay */
    int src_reg = (op >> 0) & 7;   /* src Dx/Ax */
    int size = alu_size(op);
    int is_memory_mode = (op >> 3) & 1;
    uint32_t xbit = (cpu.sr & SR_X) ? 1 : 0;

    if (is_memory_mode == 0) {
        /* SUBX Dx, Dy */
        uint32_t src = cpu.d[src_reg];
        uint32_t dest_val = cpu.d[dest_reg];
        uint32_t result;
        if (size == 1) {
            src &= 0xFF;
            dest_val &= 0xFF;
            result = (dest_val - src - xbit) & 0xFF;
            cpu.d[dest_reg] = (cpu.d[dest_reg] & 0xFFFFFF00) | result;
        } else if (size == 2) {
            src &= 0xFFFF;
            dest_val &= 0xFFFF;
            result = (dest_val - src - xbit) & 0xFFFF;
            cpu.d[dest_reg] = (cpu.d[dest_reg] & 0xFFFF0000) | result;
        } else {
            result = dest_val - src - xbit;
            cpu.d[dest_reg] = result;
        }
        set_nzvc_subx_sized(result, dest_val, src, size);
    } else {
        /* SUBX -(Ax), -(Ay) */
        cpu.a[src_reg] -= ea_step(src_reg, size);
        cpu.a[dest_reg] -= ea_step(dest_reg, size);
        uint32_t addr_x = cpu.a[dest_reg], addr_y = cpu.a[src_reg];
        uint32_t src, dest_val;
        if (size == 1) {
            src = mem_read8(addr_y) & 0xFF;
            dest_val = mem_read8(addr_x) & 0xFF;
            uint32_t result = (dest_val - src - xbit) & 0xFF;
            mem_write8(addr_x, (uint8_t)result);
            set_nzvc_subx_sized(result, dest_val, src, size);
        } else if (size == 2) {
            src = mem_read16(addr_y) & 0xFFFF;
            dest_val = mem_read16(addr_x) & 0xFFFF;
            uint32_t result = (dest_val - src - xbit) & 0xFFFF;
            mem_write16(addr_x, (uint16_t)result);
            set_nzvc_subx_sized(result, dest_val, src, size);
        } else {
            src = mem_read32(addr_y);
            dest_val = mem_read32(addr_x);
            uint32_t result = dest_val - src - xbit;
            mem_write32(addr_x, result);
            set_nzvc_subx_sized(result, dest_val, src, size);
        }
    }
}

/* MOVEQ #imm, Dn: sign-extend 8-bit immediate to 32-bit, load into Dn. Sets N,Z; clears V,C. */
void op_moveq(uint16_t op)
{
    int dest_reg = (op >> 9) & 7;
    int32_t imm = (int8_t)(op & 0xFF);
    uint32_t result = (uint32_t)imm;
    cpu.d[dest_reg] = result;
    set_nz_from_val(result, 4);
}

/* 0x9xxx: SUB or SUBX. SUBX when (op & 0x130) == 0x100 */
void dispatch_9xxx(uint16_t op)
{
    if ((op & 0x130) == 0x100) {
        op_subx(op);
        return;
    }
    int size = alu_size(op);
    op_sub_generic(op, size);
}

/* 0xBxxx: CMP */
void dispatch_Bxxx(uint16_t op)
{
    int size = alu_size(op);
    op_cmp_generic(op, size);
}

/* 0xDxxx/0xExxx/0xFxxx: ADD or ADDX. ADDX when (op & 0x130) == 0x100 */
void dispatch_add(uint16_t op)
{
    if ((op & 0x130) == 0x100) {
        op_addx(op);
        return;
    }
    int size = alu_size(op);
    op_add_generic(op, size);
}

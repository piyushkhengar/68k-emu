#include "cpu_internal.h"
#include "ea.h"
#include "alu.h"
#include "logic.h"
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

/* Dn register: bits 11-9 per 68K manual. Base unused (kept for decode_alu signature). */
static int alu_dn_reg(uint16_t op, uint8_t base)
{
    (void)base;
    return (op >> 9) & 7;
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
    if (ea_reject_byte_an(ea_mode, size)) {
        op_unimplemented(op);
        return 1;
    }
    return 0;
}

/* Decoded fields for ADD/SUB. */
typedef struct {
    int dn_reg;
    int ea_mode;
    int ea_reg;
    int size;
    uint32_t mask;
    int dir;
} alu_decoded_t;

/* Returns 0 if rejected, 1 if OK to proceed. */
static int decode_alu(uint16_t op, uint8_t base, alu_decoded_t *d)
{
    d->dn_reg = alu_dn_reg(op, base);
    ea_decode_from_op(op, (ea_decoded_t *)&d->ea_mode);
    d->dir = (op >> 9) & 1;  /* 0 = <ea> to Dn, 1 = Dn to <ea> */
    return alu_reject_byte_an(op, d->ea_mode, d->size) ? 0 : 1;
}

static int op_add_sub_generic(uint16_t op, uint8_t base)
{
    alu_decoded_t d;
    if (!decode_alu(op, base, &d))
        return 0;  /* unreachable - longjmps */

    int is_add = (base == 0xD0);
    uint32_t src, dest_val, result;
    int store_size = d.size;
    uint32_t src_for_flags;

    if (d.dir == 0) {
        /* ADD/SUB <ea>, Dn */
        src = ea_fetch_value(d.ea_mode, d.ea_reg, d.size);
        dest_val = cpu.d[d.dn_reg] & d.mask;
        result = is_add ? (dest_val + src) & d.mask : (dest_val - src) & d.mask;
        alu_store_dn(d.dn_reg, result, d.size);
        src_for_flags = src;
    } else {
        /* ADD/SUB Dn, <ea> */
        src = cpu.d[d.dn_reg];
        dest_val = ea_fetch_value(d.ea_mode, d.ea_reg, d.size);
        if (alu_is_an_word_32bit(d.ea_mode, d.size)) {
            int32_t src_se = (int32_t)(int16_t)(src & 0xFFFF);
            dest_val = cpu.a[d.ea_reg];
            result = is_add ? dest_val + src_se : dest_val - src_se;
            store_size = 4;
            src_for_flags = (uint32_t)(int32_t)(int16_t)(src & 0xFFFF);
        } else {
            src &= d.mask;
            dest_val &= d.mask;
            result = is_add ? (dest_val + src) & d.mask : (dest_val - src) & d.mask;
            src_for_flags = src;
        }
        ea_store_value(d.ea_mode, d.ea_reg, store_size, result);
    }

    if (is_add)
        set_nzvc_add_sized(result, dest_val, src_for_flags, store_size);
    else
        set_nzvc_sub_sized(result, dest_val, src_for_flags, store_size, 1);  /* SUB: X=C */
    return add_sub_cycles(d.ea_mode, d.ea_reg, d.size, d.dir);
}

/* CMP only has <ea>, Dn (no store) */
static int op_cmp_generic(uint16_t op)
{
    alu_decoded_t d;
    if (!decode_alu(op, 0xB0, &d))
        return 0;  /* unreachable - longjmps */

    uint32_t dest_val = cpu.d[d.dn_reg] & d.mask;
    uint32_t src = ea_fetch_value(d.ea_mode, d.ea_reg, d.size) & d.mask;
    uint32_t result = (dest_val - src) & d.mask;

    set_nzvc_sub_sized(result, dest_val, src, d.size, 0);  /* CMP: X not affected */
    return cmp_cycles(d.ea_mode, d.ea_reg, d.size);
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

/* Decoded fields for ADDA/SUBA/CMPA. */
typedef struct {
    int an_reg;
    int ea_mode;
    int ea_reg;
    int size;
} adda_decoded_t;

static void decode_adda(uint16_t op, adda_decoded_t *d)
{
    d->an_reg = (op >> 9) & 7;
    d->ea_mode = ea_mode_from_op(op);
    d->ea_reg = ea_reg_from_op(op);
    d->size = ((op >> 6) & 7) == 7 ? 4 : 2;  /* 111=long, 011=word */
}

static int op_adda(uint16_t op)
{
    adda_decoded_t d;
    decode_adda(op, &d);

    uint32_t src = ea_fetch_value(d.ea_mode, d.ea_reg, d.size);
    uint32_t dest = cpu.a[d.an_reg];
    uint32_t result;

    if (d.size == 2)
        src = (uint32_t)(int32_t)(int16_t)(src & 0xFFFF);
    result = dest + src;
    cpu.a[d.an_reg] = result;
    return add_sub_cycles(d.ea_mode, d.ea_reg, d.size, 0);
}

static int op_suba(uint16_t op)
{
    adda_decoded_t d;
    decode_adda(op, &d);

    uint32_t src = ea_fetch_value(d.ea_mode, d.ea_reg, d.size);
    uint32_t dest = cpu.a[d.an_reg];
    uint32_t result;

    if (d.size == 2)
        src = (uint32_t)(int32_t)(int16_t)(src & 0xFFFF);
    result = dest - src;
    cpu.a[d.an_reg] = result;
    return add_sub_cycles(d.ea_mode, d.ea_reg, d.size, 0);
}

static int op_cmpa(uint16_t op)
{
    adda_decoded_t d;
    decode_adda(op, &d);

    uint32_t dest = cpu.a[d.an_reg];
    uint32_t src = ea_fetch_value(d.ea_mode, d.ea_reg, d.size);
    uint32_t result;

    if (d.size == 2)
        src = (uint32_t)(int32_t)(int16_t)(src & 0xFFFF);
    result = (dest - src) & 0xFFFFFFFF;
    set_nzvc_sub_sized(result, dest, src, 4, 0);  /* CMPA: X not affected */
    return cmp_cycles(d.ea_mode, d.ea_reg, d.size);
}

/* Decoded fields for ADDX/SUBX. Format: 1101/1001 Rx 1 SIZE 0 0 R/M Ry. */
typedef struct {
    int dest_reg;
    int src_reg;
    int size;
    int is_memory_mode;
    uint32_t mask;
} addx_decoded_t;

static void decode_addx(uint16_t op, addx_decoded_t *d)
{
    d->dest_reg = (op >> 9) & 7;
    d->src_reg = (op >> 0) & 7;
    d->size = decode_size_bits_6_7(op);
    d->is_memory_mode = (op >> 3) & 1;
    d->mask = size_mask(d->size);
}

/* ADDX/SUBX: dest = dest op src op X. Z: cleared if nonzero. */
static int op_addx_subx(uint16_t op, int is_add)
{
    addx_decoded_t d;
    decode_addx(op, &d);
    uint32_t xbit = (cpu.sr & SR_X) ? 1 : 0;

    if (d.is_memory_mode == 0) {
        /* ADDX/SUBX Dy, Dx (register mode) */
        uint32_t src = cpu.d[d.src_reg] & d.mask;
        uint32_t dest_val = cpu.d[d.dest_reg] & d.mask;
        uint32_t result = is_add ? (dest_val + src + xbit) & d.mask : (dest_val - src - xbit) & d.mask;
        alu_store_dn(d.dest_reg, result, d.size);
        if (is_add)
            set_nzvc_addx_sized(result, dest_val, src, d.size);
        else
            set_nzvc_subx_sized(result, dest_val, src, d.size);
    } else {
        /* ADDX/SUBX -(Ay), -(Ax): decrement both, fetch, op, store */
        cpu.a[d.src_reg] -= ea_step(d.src_reg, d.size);
        cpu.a[d.dest_reg] -= ea_step(d.dest_reg, d.size);
        uint32_t addr_x = cpu.a[d.dest_reg];
        uint32_t addr_y = cpu.a[d.src_reg];
        uint32_t src = alu_mem_read_sized(addr_y, d.size);
        uint32_t dest_val = alu_mem_read_sized(addr_x, d.size);
        uint32_t result = is_add ? (dest_val + src + xbit) & d.mask : (dest_val - src - xbit) & d.mask;
        alu_mem_write_sized(addr_x, d.size, result);
        if (is_add)
            set_nzvc_addx_sized(result, dest_val, src, d.size);
        else
            set_nzvc_subx_sized(result, dest_val, src, d.size);
    }
    return addx_subx_cycles(d.is_memory_mode, d.size);
}

/* Decoded fields for MOVEQ. */
typedef struct {
    int dest_reg;
    int32_t imm;
} moveq_decoded_t;

static void decode_moveq(uint16_t op, moveq_decoded_t *d)
{
    d->dest_reg = (op >> 9) & 7;
    d->imm = (int8_t)(op & 0xFF);
}

/* MOVEQ #imm, Dn: sign-extend 8-bit immediate to 32-bit, load into Dn. Sets N,Z; clears V,C. */
int op_moveq(uint16_t op)
{
    moveq_decoded_t d;
    decode_moveq(op, &d);
    uint32_t result = (uint32_t)d.imm;
    cpu.d[d.dest_reg] = result;
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
    return op_add_sub_generic(op, 0x90);
}

/* 0xBxxx: CMP, CMPA, or EOR. CMPA when opmode 011/111. EOR when bit 8 set (1ss vs 0ss). */
int dispatch_Bxxx(uint16_t op)
{
    if (alu_is_adda_suba_cmpa(op))
        return op_cmpa(op);
    if (op & 0x0100)  /* EOR has 1ss in bits 8-6, CMP has 0ss */
        return op_eor(op);
    return op_cmp_generic(op);
}

/* 0xDxxx/0xExxx/0xFxxx: ADD, ADDA, or ADDX. ADDA when opmode 011/111; ADDX when (op & 0x130)==0x100. */
int dispatch_add(uint16_t op)
{
    if (alu_is_adda_suba_cmpa(op))
        return op_adda(op);
    if ((op & 0x130) == 0x100)
        return op_addx_subx(op, 1);
    return op_add_sub_generic(op, 0xD0);
}

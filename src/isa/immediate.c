/*
 * ADDI, SUBI, CMPI (immediate) and ADDQ, SUBQ (quick) instructions.
 * ADDI/SUBI/CMPI: 0x04xx, 0x06xx, 0x0Cxx. Immediate data follows opcode.
 * ADDQ/SUBQ: 0x50xx, 0x51xx. Data 1-8 in bits 11-9 (0=8).
 */

#include "bit.h"
#include "cpu_internal.h"
#include "movep.h"
#include "branch.h"
#include "ea.h"
#include "memory.h"
#include "timing.h"

/* Fixed base cycle counts for immediate arithmetic instructions. */
#define CYCLES_ADDI_SUBI_DN_BW   8   /* ADDI/SUBI Dn byte/word */
#define CYCLES_ADDI_SUBI_DN_L   16   /* ADDI/SUBI Dn long */
#define CYCLES_ADDI_SUBI_MEM_BW 12   /* ADDI/SUBI memory byte/word base */
#define CYCLES_ADDI_SUBI_MEM_L  20   /* ADDI/SUBI memory long base */
#define CYCLES_CMPI_DN_BW        8   /* CMPI Dn byte/word */
#define CYCLES_CMPI_DN_L        14   /* CMPI Dn long */
#define CYCLES_CMPI_MEM_BW       8   /* CMPI memory byte/word base */
#define CYCLES_CMPI_MEM_L       12   /* CMPI memory long base */
#define CYCLES_ORI_ANDI_EORI_CCR_SR  20

static uint32_t fetch_imm(int size)
{
    if (size == 1) {
        uint16_t w = fetch16();
        pending_cycles += 4;
        return w & 0xFF;
    }
    if (size == 2) {
        uint16_t w = fetch16();
        pending_cycles += 4;
        return w & 0xFFFF;
    }
    pending_cycles += 8;
    return fetch32();
}

/* ADDI/SUBI/CMPI: An not allowed as destination. */
static int imm_reject_an(uint16_t op, int ea_mode)
{
    if (ea_is_an(ea_mode)) {
        op_unimplemented(op);
        return 1;
    }
    return 0;
}

/* Returns 0 if rejected, 1 if OK to proceed. */
static int decode_imm(uint16_t op, ea_decoded_t *d)
{
    ea_decode_from_op(op, d);
    return imm_reject_an(op, d->ea_mode) ? 0 : 1;
}

/* ADDI #imm, <ea>: dest = dest + imm. 0x06xx */
static int op_addi(uint16_t op)
{
    ea_decoded_t d;
    if (!decode_imm(op, &d))
        return 0;

    uint32_t imm = fetch_imm(d.size);
    ea_rmw_t rmw;
    uint32_t dest = ea_read_rmw(d.ea_mode, d.ea_reg, d.size, &rmw) & d.mask;
    uint32_t result = (dest + imm) & d.mask;

    ea_write_rmw(&rmw, result);
    set_nzvc_add_sized(result, dest, imm, d.size);
    if (d.ea_mode == 0)
        return (d.size == 4) ? CYCLES_ADDI_SUBI_DN_L : CYCLES_ADDI_SUBI_DN_BW;
    return ((d.size == 4) ? CYCLES_ADDI_SUBI_MEM_L : CYCLES_ADDI_SUBI_MEM_BW) + ea_cycles(d.ea_mode, d.ea_reg, d.size);
}

/* SUBI #imm, <ea>: dest = dest - imm. 0x04xx */
static int op_subi(uint16_t op)
{
    ea_decoded_t d;
    if (!decode_imm(op, &d))
        return 0;

    uint32_t imm = fetch_imm(d.size);
    ea_rmw_t rmw;
    uint32_t dest = ea_read_rmw(d.ea_mode, d.ea_reg, d.size, &rmw) & d.mask;
    uint32_t result = (dest - imm) & d.mask;

    ea_write_rmw(&rmw, result);
    set_nzvc_sub_sized(result, dest, imm, d.size, 1);  /* SUBI: X=C */
    if (d.ea_mode == 0)
        return (d.size == 4) ? CYCLES_ADDI_SUBI_DN_L : CYCLES_ADDI_SUBI_DN_BW;
    return ((d.size == 4) ? CYCLES_ADDI_SUBI_MEM_L : CYCLES_ADDI_SUBI_MEM_BW) + ea_cycles(d.ea_mode, d.ea_reg, d.size);
}

/* CMPI #imm, <ea>: compare, no store. 0x0Cxx. X not affected. */
static int op_cmpi(uint16_t op)
{
    ea_decoded_t d;
    if (!decode_imm(op, &d))
        return 0;

    uint32_t imm = fetch_imm(d.size);
    uint32_t dest = ea_fetch_value(d.ea_mode, d.ea_reg, d.size) & d.mask;
    uint32_t result = (dest - imm) & d.mask;

    set_nzvc_sub_sized(result, dest, imm, d.size, 0);  /* CMPI: X not affected */
    if (d.ea_mode == 0)
        return (d.size == 4) ? CYCLES_CMPI_DN_L : CYCLES_CMPI_DN_BW;
    return ((d.size == 4) ? CYCLES_CMPI_MEM_L : CYCLES_CMPI_MEM_BW) + ea_cycles(d.ea_mode, d.ea_reg, d.size);
}

/* ORI #imm, <ea>: dest = dest | imm. 0x00xx. An not allowed. */
static int op_ori(uint16_t op)
{
    ea_decoded_t d;
    if (!decode_imm(op, &d))
        return 0;

    uint32_t imm = fetch_imm(d.size);
    ea_rmw_t rmw;
    uint32_t dest = ea_read_rmw(d.ea_mode, d.ea_reg, d.size, &rmw) & d.mask;
    uint32_t result = (dest | imm) & d.mask;

    ea_write_rmw(&rmw, result);
    set_nz_from_val(result, d.size);
    cpu.sr &= ~(SR_V | SR_C);
    if (d.ea_mode == 0)
        return (d.size == 4) ? CYCLES_ADDI_SUBI_DN_L : CYCLES_ADDI_SUBI_DN_BW;
    return ((d.size == 4) ? CYCLES_ADDI_SUBI_MEM_L : CYCLES_ADDI_SUBI_MEM_BW) + ea_cycles(d.ea_mode, d.ea_reg, d.size);
}

/* ANDI #imm, <ea>: dest = dest & imm. 0x02xx. An not allowed. */
static int op_andi(uint16_t op)
{
    ea_decoded_t d;
    if (!decode_imm(op, &d))
        return 0;

    uint32_t imm = fetch_imm(d.size);
    ea_rmw_t rmw;
    uint32_t dest = ea_read_rmw(d.ea_mode, d.ea_reg, d.size, &rmw) & d.mask;
    uint32_t result = (dest & imm) & d.mask;

    ea_write_rmw(&rmw, result);
    set_nz_from_val(result, d.size);
    cpu.sr &= ~(SR_V | SR_C);
    if (d.ea_mode == 0)
        return (d.size == 4) ? CYCLES_ADDI_SUBI_DN_L : CYCLES_ADDI_SUBI_DN_BW;
    return ((d.size == 4) ? CYCLES_ADDI_SUBI_MEM_L : CYCLES_ADDI_SUBI_MEM_BW) + ea_cycles(d.ea_mode, d.ea_reg, d.size);
}

/* EORI #imm, <ea>: dest = dest ^ imm. 0x0Axx. An not allowed. */
static int op_eori(uint16_t op)
{
    ea_decoded_t d;
    if (!decode_imm(op, &d))
        return 0;

    uint32_t imm = fetch_imm(d.size);
    ea_rmw_t rmw;
    uint32_t dest = ea_read_rmw(d.ea_mode, d.ea_reg, d.size, &rmw) & d.mask;
    uint32_t result = (dest ^ imm) & d.mask;

    ea_write_rmw(&rmw, result);
    set_nz_from_val(result, d.size);
    cpu.sr &= ~(SR_V | SR_C);
    if (d.ea_mode == 0)
        return (d.size == 4) ? CYCLES_ADDI_SUBI_DN_L : CYCLES_ADDI_SUBI_DN_BW;
    return ((d.size == 4) ? CYCLES_ADDI_SUBI_MEM_L : CYCLES_ADDI_SUBI_MEM_BW) + ea_cycles(d.ea_mode, d.ea_reg, d.size);
}

/* ORI/ANDI/EORI to CCR: byte immediate, CCR = low byte of SR. 0x003C, 0x023C, 0x0A3C. */
static int op_ori_ccr(uint16_t op)
{
    (void)op;
    uint8_t imm = fetch16() & 0xFF;
    uint8_t ccr = cpu.sr & 0x1F;
    cpu.sr = (cpu.sr & 0xFF00) | ((ccr | imm) & 0x1F);
    return CYCLES_ORI_ANDI_EORI_CCR_SR;
}

static int op_andi_ccr(uint16_t op)
{
    (void)op;
    uint8_t imm = fetch16() & 0xFF;
    uint8_t ccr = cpu.sr & 0x1F;
    cpu.sr = (cpu.sr & 0xFF00) | ((ccr & imm) & 0x1F);
    return CYCLES_ORI_ANDI_EORI_CCR_SR;
}

static int op_eori_ccr(uint16_t op)
{
    (void)op;
    uint8_t imm = fetch16() & 0xFF;
    uint8_t ccr = cpu.sr & 0x1F;
    cpu.sr = (cpu.sr & 0xFF00) | ((ccr ^ imm) & 0x1F);
    return CYCLES_ORI_ANDI_EORI_CCR_SR;
}

/* ORI/ANDI/EORI to SR: word immediate.  Privileged.
 *
 * On a stock 68000 these cause a privilege violation from user mode.
 * AmigaOS's Exec::Supervisor() relies on exactly this: user code
 * executes ORI #$2000,SR which traps to the vector 8 handler, which
 * completes the supervisor mode switch.  We must honour the trap so
 * that the OS's Supervisor() mechanism works correctly. */
static int op_ori_sr(uint16_t op)
{
    (void)op;
    if (!require_supervisor())
        return 0;
    uint16_t imm = fetch16();
    cpu.sr = (cpu.sr | imm) & SR_VALID;
    return CYCLES_ORI_ANDI_EORI_CCR_SR;
}

static int op_andi_sr(uint16_t op)
{
    (void)op;
    if (!require_supervisor())
        return 0;
    uint16_t imm = fetch16();
    uint16_t old_sr = cpu.sr;
    cpu.sr = (cpu.sr & imm) & SR_VALID;
    /* If S bit changed 1→0, switch stacks */
    if ((old_sr & SR_S) && !(cpu.sr & SR_S)) {
        cpu.ssp = cpu.a[7];
        cpu.a[7] = cpu.usp;
    }
    return CYCLES_ORI_ANDI_EORI_CCR_SR;
}

static int op_eori_sr(uint16_t op)
{
    (void)op;
    if (!require_supervisor())
        return 0;
    uint16_t imm = fetch16();
    uint16_t old_sr = cpu.sr;
    cpu.sr = (cpu.sr ^ imm) & SR_VALID;
    if ((old_sr & SR_S) != (cpu.sr & SR_S)) {
        if (cpu.sr & SR_S) { cpu.usp = cpu.a[7]; cpu.a[7] = cpu.ssp; }
        else               { cpu.ssp = cpu.a[7]; cpu.a[7] = cpu.usp; }
    }
    return CYCLES_ORI_ANDI_EORI_CCR_SR;
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
static int decode_addq(uint16_t op, addq_decoded_t *d)
{
    d->data = (op >> 9) & 7;
    if (d->data == 0)
        d->data = 8;
    ea_decode_from_op(op, (ea_decoded_t *)&d->ea_mode);
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
    if (!decode_addq(op, &d))
        return 0;

    if (d.ea_mode == 1) {
        /* Address register: W/L only, no flags, 32-bit result */
        uint32_t dest = cpu.a[d.ea_reg];
        uint32_t result = is_sub ? dest - d.data : dest + d.data;
        cpu.a[d.ea_reg] = result;
        if (d.ea_reg == 7)
            sync_a7_to_sp();
        return (d.size == 4) ? 6 : 8;
    }

    ea_rmw_t rmw;
    uint32_t dest = ea_read_rmw(d.ea_mode, d.ea_reg, d.size, &rmw) & d.mask;
    uint32_t result = is_sub ? (dest - d.data) & d.mask : (dest + d.data) & d.mask;

    ea_write_rmw(&rmw, result);
    if (is_sub)
        set_nzvc_sub_sized(result, dest, (uint32_t)d.data, d.size, 1);  /* SUBQ: X=C */
    else
        set_nzvc_add_sized(result, dest, (uint32_t)d.data, d.size);

    if (d.ea_mode == 0)
        return (d.size == 4) ? 8 : 4;
    return ((d.size == 4) ? 12 : 8) + ea_cycles(d.ea_mode, d.ea_reg, d.size);
}

/* 0x0xxx: ORI (0x00), ANDI (0x02), SUBI (0x04), ADDI (0x06), EORI (0x0A), CMPI (0x0C).
 * ORI/ANDI/EORI to CCR (0x3C) and SR (0x7C). */
int dispatch_0xxx(uint16_t op)
{
    /* MOVEP: 0x0108, 0x0148, 0x0188, 0x01C8 (and Dn/An variants). Check before high-nibble dispatch.
     * 68060: MOVEP was removed — raise Line-1111 (unimplemented instruction, vector 11). */
    if (cpu.features.has_movep) {
        int c = op_movep(op);
        if (c) return c;
    } else {
        uint16_t base = op & 0xF1F8;
        if (base == 0x0108 || base == 0x0148 || base == 0x0188 || base == 0x01C8) {
            cpu.pc -= 2;
            cpu_take_exception(LINE1111_VECTOR, 4);
            return 0;  /* unreachable */
        }
    }
    /* BTST/BCHG/BCLR/BSET Dn: check before ea_field 0x3C (ORI/ANDI/EORI to CCR) which shares EA #imm. */
    if ((op & 0xF1C0) >= 0x0100 && (op & 0xF1C0) <= 0x01C0)
        return op_bit_dn(op);
    int ea_byte = op & 0x00FF;   /* full lower byte to distinguish CCR (0x3C) vs SR (0x7C) */
    int ea_field = op & 0x003F;
    int high = (op >> 8) & 0x0F;
    if (ea_byte == 0x3C) {
        if (high == 0x00) return op_ori_ccr(op);
        if (high == 0x02) return op_andi_ccr(op);
        if (high == 0x0A) return op_eori_ccr(op);
        return op_unimplemented(op);  /* SUBI/ADDI/CMPI to CCR invalid */
    }
    if (ea_byte == 0x7C) {
        if (high == 0x00) return op_ori_sr(op);
        if (high == 0x02) return op_andi_sr(op);
        if (high == 0x0A) return op_eori_sr(op);
        return op_unimplemented(op);  /* SUBI/ADDI/CMPI to SR invalid */
    }

    /* Bit ops #imm: 0x08xx (BTST/BCHG/BCLR/BSET with #imm count). */
    if (high == 0x08)
        return op_bit_imm(op);
    /* EORI #imm: all sizes (byte/word/long). CCR/SR variants handled above. */
    if (high == 0x0A)
        return op_eori(op);
    if (high == 0x00) return op_ori(op);
    if (high == 0x02) return op_andi(op);
    if (high == 0x04) return op_subi(op);
    if (high == 0x06) return op_addi(op);
    if (high == 0x0C) return op_cmpi(op);
    return op_unimplemented(op);
}

/* DBcc: 0x50C0-0x50FF. Decrement Dn (word); if condition false and Dn != -1, branch.
 * 16-bit displacement: base = addr of extension word; after fetch16, target = (PC-2) + disp. */
static int op_dbcc(uint16_t op)
{
    uint8_t cond = (op >> 8) & 0x0F;
    int dn = op & 7;
    int32_t disp = (int16_t)fetch16();

    if (branch_condition_met(cond))
        return dbcc_cycles(0);   /* Condition true: terminate (12 cycles) */

    uint16_t dn_val = (uint16_t)(cpu.d[dn] & 0xFFFF);
    int16_t new_val = (int16_t)(dn_val - 1);
    store_dn(dn, (uint32_t)(uint16_t)new_val, 2);

    if (new_val != -1) {
        cpu.pc += disp - 2;
        if (cpu.pc & 1) {
            pending_cycles += 2;
            cpu_take_addr_err(cpu.pc, op);
        }
        return dbcc_cycles(1);
    }
    return dbcc_cycles(2);  /* count expired (14 cycles) */
}

/* Scc: 0x5Cxx. Set byte to 0xFF if condition true, else 0x00. An (mode 1) not allowed. */
static int op_scc(uint16_t op)
{
    int ea_mode = ea_mode_from_op(op);
    int ea_reg = ea_reg_from_op(op);
    uint8_t cond = (op >> 8) & 0x0F;

    if (ea_is_an(ea_mode)) {
        op_unimplemented(op);
        return 0;
    }

    int cond_true = branch_condition_met(cond);
    uint8_t val = cond_true ? 0xFF : 0x00;
    ea_store_value(ea_mode, ea_reg, 1, val);
    return scc_cycles_full(ea_mode, ea_reg, cond_true);
}

/* TRAPcc: 0x50Fx–0x5FFx (bits 7-3 = 11111, bits 2-0 = sub-code). 68020+ only.
 *
 * This instruction is the 68020 generalisation of TRAPV: it takes a trap
 * (via the TRAPV vector, vector 7) when the chosen condition is true.  When
 * the condition is false the instruction is a no-op — it just consumes PC
 * past any optional operand and returns.
 *
 * Three sub-codes in bits 2-0 select the operand size:
 *   2 (FA): one word follows — TRAPcc.W #<word>
 *   3 (FB): one long follows — TRAPcc.L #<long>
 *   4 (FC): no operand       — TRAPcc
 *
 * The optional operand is provided purely for the trap handler to read from
 * the exception stack frame; the processor itself doesn't use it.  We just
 * skip (fetch) it so that PC is left pointing at the next instruction.
 *
 * Cycle counts (not-taken): 4 base + 4 per extra word fetched.
 * Trap-taken: exception_cycles(7) = 34, with pre-fault cycles for any fetch. */
static int op_trapcc(uint16_t op)
{
    int sub          = op & 7;
    int fetch_cycles = 0;

    if (sub == 2) {
        fetch16();
        fetch_cycles = 4;   /* one extra word read */
    } else if (sub == 3) {
        fetch32();
        fetch_cycles = 8;   /* one extra long read */
    }
    /* sub == 4: no operand, fetch_cycles stays 0 */

    uint8_t cond = (op >> 8) & 0x0F;
    if (branch_condition_met(cond)) {
        /* Condition true: push exception frame and jump through vector 7.
         * Pass fetch_cycles so the exception handler accounts for the
         * cycles already spent reading the optional operand. */
        cpu_take_exception(TRAPV_VECTOR, fetch_cycles);
        return 0;
    }
    /* Condition false: nothing to do; PC already past the operand. */
    return 4 + fetch_cycles;
}

/* 0x5xxx: DBcc (bits 7-3=11001), TRAPcc (bits 7-3=11111, 68020+),
 * Scc (size 11, other), ADDQ, SUBQ. */
int dispatch_5xxx(uint16_t op)
{
    if ((op & 0xF0C0) == 0x50C0) {  /* Size 11 in bits 7-6 */
        if ((op & 0x00F8) == 0x00C8)  /* DBcc: bits 7-3 = 11001 */
            return op_dbcc(op);
        /* TRAPcc: bits 7-3 = 11111. Sub-code in bits 2-0 must be 2, 3, or 4. */
        if (cpu.features.has_trapcc && (op & 0x00F8) == 0x00F8) {
            int sub = op & 7;
            if (sub >= 2 && sub <= 4)
                return op_trapcc(op);
        }
        return op_scc(op);             /* Scc: EA in bits 5-0 */
    }
    if ((op & 0x0100) == 0)
        return op_addq_subq(op, 0);   /* ADDQ */
    return op_addq_subq(op, 1);       /* SUBQ */
}

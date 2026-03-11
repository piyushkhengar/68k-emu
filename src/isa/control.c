#include "cpu_internal.h"
#include "control.h"
#include "ea.h"
#include "logic.h"
#include "memory.h"
#include "movem.h"
#include "timing.h"

#define CYCLES_STOP  4
#define CYCLES_RESET 132

/* RESET: 0x4E70. Privileged. Assert external RESET (no-op in emulator). */
static int op_reset(uint16_t op)
{
    (void)op;
    if (!require_supervisor())
        return 0;
    return CYCLES_RESET;
}

/* STOP: 0x4E72. Privileged. Load SR from immediate, then halt. */
static int op_stop(uint16_t op)
{
    (void)op;
    uint16_t imm = fetch16();
    if (!(imm & 0x2000)) {  /* S-bit clear = user mode */
        cpu_take_exception(PRIVILEGE_VECTOR, 4);
        return 0;
    }
    cpu.sr = imm;
    cpu.halted = 1;
    return CYCLES_STOP;
}

/* TRAPV: 0x4E76. Trap on overflow (vector 7). */
static int op_trapv(uint16_t op)
{
    (void)op;
    if (cpu.sr & SR_V) {
        cpu_take_exception(TRAPV_VECTOR, 4);
        return 0;
    }
    return 4;
}

/* CHK Dn, <ea>: 0x4180-0x41BF. Bounds check; trap vector 6 if Dn < 0 or Dn > (EA). */
static int op_chk(uint16_t op)
{
    int dn = (op >> 9) & 7;
    int ea_mode = ea_mode_from_op(op);
    int ea_reg = ea_reg_from_op(op);

    int32_t dn_val = (int32_t)(int16_t)(cpu.d[dn] & 0xFFFF);
    int32_t bound = (int32_t)(int16_t)(ea_fetch_value(ea_mode, ea_reg, 2) & 0xFFFF);

    if (dn_val < 0 || dn_val > bound) {
        cpu.sr &= ~SR_N;
        if (dn_val < 0)
            cpu.sr |= SR_N;  /* N=1 if Dn < 0 */
        /* N=0 if Dn > bound */
        cpu_take_exception(CHK_VECTOR, 4);
        return 0;
    }
    cpu.sr &= ~SR_N;  /* In bounds: N=0 */
    return chk_cycles(ea_mode, ea_reg);
}

/* MOVE USP, An: 0x4E68-0x4E6F (ProcessorTests: MOVEfromUSP). Supervisor only. */
static int op_move_usp_to_an(uint16_t op)
{
    if (!require_supervisor())
        return 0;
    int an = op & 7;
    cpu.a[an] = cpu.usp;
    if (an == 7 && (cpu.sr & 0x2000))
        cpu.ssp = cpu.a[7];  /* A7 = SSP in supervisor mode */
    return 4;
}

/* MOVE An, USP: 0x4E60-0x4E67 (ProcessorTests: MOVEtoUSP). Supervisor only. */
static int op_move_an_to_usp(uint16_t op)
{
    if (!require_supervisor())
        return 0;
    int an = op & 7;
    cpu.usp = cpu.a[an];
    if (an == 7 && !(cpu.sr & 0x2000))
        cpu.a[7] = cpu.usp;  /* User mode: A7 = USP */
    return 4;
}

/* NOP: no operation. 0x4E71. */
static int op_nop(uint16_t op)
{
    (void)op;
    return CYCLES_NOP;
}

/* RTR: pop CCR (low byte of SR), then pop PC. 0x4E77. */
static int op_rtr(uint16_t op)
{
    (void)op;
    uint32_t sp = cpu_sp();
    uint8_t ccr = (uint8_t)(mem_read16(sp) & 0xFF);
    cpu.sr = (cpu.sr & 0xFF00) | ccr;
    cpu.pc = mem_read32(sp + 2);
    cpu_sp_set(sp + 6);
    return 20;  /* Motorola: RTR = 20 cycles */
}

/* RTS: pop return address from stack, jump to it. 0x4E75. */
static int op_rts(uint16_t op)
{
    (void)op;
    uint32_t sp = cpu_sp();
    cpu.pc = mem_read32(sp);
    cpu_sp_set(sp + 4);
    return CYCLES_RTS;
}

#define CYCLES_RTE  20

/* TRAP #n: software interrupt. 0x4E40-0x4E4F. Vector 32+n. */
static int op_trap(uint16_t op)
{
    int n = op & 0x0F;
    cpu_take_exception(32 + n, 4);  /* 4 cycles for opcode fetch */
    return 0;  /* unreachable */
}

/* RTE: return from exception. 0x4E73. Supervisor only. */
static int op_rte(uint16_t op)
{
    (void)op;
    if (!require_supervisor())
        return 0;
    uint32_t sp = cpu.ssp;
    cpu.sr = mem_read16(sp);
    cpu.pc = mem_read32(sp + 2);
    cpu.ssp = sp + 6;
    cpu.a[7] = (cpu.sr & 0x2000) ? cpu.ssp : cpu.usp;
    return CYCLES_RTE;
}

/* Returns 0 if rejected, 1 if OK to proceed. */
static int decode_not(uint16_t op, ea_decoded_t *d)
{
    ea_decode_from_op(op, d);
    if (ea_is_an(d->ea_mode)) {
        op_unimplemented(op);
        return 0;
    }
    return 1;
}

/* NOT <ea>: one's complement. 0x46xx. An not allowed. */
static int op_not(uint16_t op)
{
    ea_decoded_t d;
    if (!decode_not(op, &d))
        return 0;

    uint32_t val = ea_fetch_value(d.ea_mode, d.ea_reg, d.size) & d.mask;
    uint32_t result = (~val) & d.mask;
    ea_store_value(d.ea_mode, d.ea_reg, d.size, result);
    set_nz_from_val(result, d.size);
    return add_sub_cycles(d.ea_mode, d.ea_reg, d.size, 1);
}

/* Decode EA for JMP/JSR. Fills *addr_out, *ea_mode, *ea_reg on success.
 * On invalid EA (Dn, An, #imm), calls op_unimplemented (does not return). */
static void decode_ea_addr_jmp_jsr(uint16_t op, uint32_t *addr_out, int *ea_mode, int *ea_reg)
{
    *ea_mode = ea_mode_from_op(op);
    *ea_reg = ea_reg_from_op(op);
    if (ea_invalid_for_jmp_jsr(*ea_mode, *ea_reg))
        op_unimplemented(op);
    if (!ea_address_no_fetch(*ea_mode, *ea_reg, addr_out))
        op_unimplemented(op);
}

/* PEA <ea>: push effective address. 0x4848-0x487F. Invalid: Dn, An, #imm. */
static int op_pea(uint16_t op)
{
    int ea_mode = ea_mode_from_op(op);
    int ea_reg = ea_reg_from_op(op);

    if (ea_invalid_for_lea(ea_mode, ea_reg))
        return op_unimplemented(op);
    uint32_t addr;
    if (!ea_address_no_fetch(ea_mode, ea_reg, &addr))
        return op_unimplemented(op);

    uint32_t sp = cpu_sp() - 4;
    mem_write32(sp, addr);
    cpu_sp_set(sp);
    return pea_cycles(ea_mode, ea_reg);
}

/* LEA <ea>, An. 0x41xx. Invalid: Dn, An, (An)+, -(An), #imm. */
static int op_lea(uint16_t op)
{
    int an_reg = (op >> 9) & 7;
    int ea_mode = ea_mode_from_op(op);
    int ea_reg = ea_reg_from_op(op);

    if (ea_invalid_for_lea(ea_mode, ea_reg))
        return op_unimplemented(op);

    uint32_t addr;
    if (!ea_address_no_fetch(ea_mode, ea_reg, &addr))
        return op_unimplemented(op);

    cpu.a[an_reg] = addr;
    return lea_cycles(ea_mode, ea_reg);
}

/* JMP <ea>. 0x4EC0-0x4EFF. Invalid: Dn, An, #imm. */
static int op_jmp(uint16_t op)
{
    uint32_t addr;
    int ea_mode, ea_reg;
    decode_ea_addr_jmp_jsr(op, &addr, &ea_mode, &ea_reg);
    cpu.pc = addr;
    return jmp_cycles(ea_mode, ea_reg);
}

/* JSR <ea>. 0x4E80-0x4EBF. Invalid: Dn, An, #imm. */
static int op_jsr(uint16_t op)
{
    uint32_t addr;
    int ea_mode, ea_reg;
    decode_ea_addr_jmp_jsr(op, &addr, &ea_mode, &ea_reg);
    uint32_t sp = cpu_sp() - 4;
    mem_write32(sp, cpu.pc);
    cpu_sp_set(sp);
    cpu.pc = addr;
    return jsr_cycles(ea_mode, ea_reg);
}

/* TAS <ea>: test and set byte. 0x4AC0-0x4AFF. Read-modify-write: test (set N,Z), set bit 7, store. An not allowed. */
static int op_tas(uint16_t op)
{
    int ea_mode = ea_mode_from_op(op);
    int ea_reg = ea_reg_from_op(op);
    if (ea_mode == 1)
        return op_unimplemented(op);
    uint8_t val = (uint8_t)(ea_fetch_value(ea_mode, ea_reg, 1) & 0xFF);
    cpu.sr &= ~(SR_N | SR_Z | SR_V | SR_C);
    if (val == 0)
        cpu.sr |= SR_Z;
    if (val & 0x80)
        cpu.sr |= SR_N;
    uint8_t result = val | 0x80;
    ea_store_value(ea_mode, ea_reg, 1, result);
    return 4 + ea_cycles(ea_mode, ea_reg, 1) * 2;  /* read + write */
}

/* TST <ea>. 0x4Axx. Compare with zero, set N,Z, clear V,C. */
static int op_tst(uint16_t op)
{
    int ea_mode = ea_mode_from_op(op);
    int ea_reg = ea_reg_from_op(op);
    int size = decode_size_bits_6_7(op);

    uint32_t val = ea_fetch_value(ea_mode, ea_reg, size);
    set_nz_from_val(val, size);
    cpu.sr &= ~(SR_V | SR_C);
    return tst_cycles(ea_mode, ea_reg, size);
}

/* NEG <ea>: dest = 0 - dest. 0x44xx. Data alterable. Sets N,Z,V,C,X. */
static int op_neg(uint16_t op)
{
    ea_decoded_t d;
    ea_decode_from_op(op, &d);
    if (ea_is_an(d.ea_mode))
        return op_unimplemented(op);
    uint32_t dest = ea_fetch_value(d.ea_mode, d.ea_reg, d.size) & d.mask;
    uint32_t result = (0 - dest) & d.mask;
    ea_store_value(d.ea_mode, d.ea_reg, d.size, result);
    set_nzvc_sub_sized(result, 0, dest, d.size);
    return add_sub_cycles(d.ea_mode, d.ea_reg, d.size, 1);
}

/* NEGX <ea>: dest = 0 - dest - X. 0x40xx (excl. 0x40C0-0x43FF MOVE from SR). Z: cleared if result nonzero, else unchanged. */
static int op_negx(uint16_t op)
{
    ea_decoded_t d;
    ea_decode_from_op(op, &d);
    if (ea_is_an(d.ea_mode))
        return op_unimplemented(op);
    uint8_t x_in = (cpu.sr & SR_X) ? 1 : 0;
    uint32_t dest = ea_fetch_value(d.ea_mode, d.ea_reg, d.size) & d.mask;
    uint32_t result = (0 - dest - x_in) & d.mask;
    ea_store_value(d.ea_mode, d.ea_reg, d.size, result);
    cpu.sr &= ~(SR_N | SR_V | SR_C | SR_X);
    if (result != 0)
        cpu.sr &= ~SR_Z;
    if (d.size == 1 && (result & 0x80))
        cpu.sr |= SR_N;
    else if (d.size == 2 && (result & 0x8000))
        cpu.sr |= SR_N;
    else if (d.size == 4 && (result & 0x80000000))
        cpu.sr |= SR_N;
    if (dest != 0 || x_in)
        cpu.sr |= SR_C | SR_X;
    /* V: overflow when 0 - dest - X overflows */
    if (d.size == 1) {
        int32_t r = (int32_t)(int8_t)result, dest_s = (int32_t)(int8_t)dest;
        if ((dest_s > 0 && r < 0) || (dest_s == 0 && x_in && r != 0))
            cpu.sr |= SR_V;
    } else if (d.size == 2) {
        int32_t r = (int32_t)(int16_t)result, dest_s = (int32_t)(int16_t)dest;
        if ((dest_s > 0 && r < 0) || (dest_s == 0 && x_in && r != 0))
            cpu.sr |= SR_V;
    } else {
        int32_t r = (int32_t)result, dest_s = (int32_t)dest;
        if ((dest_s > 0 && r < 0) || (dest_s == 0 && x_in && r != 0))
            cpu.sr |= SR_V;
    }
    return add_sub_cycles(d.ea_mode, d.ea_reg, d.size, 1);
}

/* MOVE.W SR, <ea>. 0x40C0-0x43FF. Dest EA in bits 5-0. Data alterable only. Unprivileged on 68000. */
static int op_move_from_sr(uint16_t op)
{
    int ea_mode = ea_mode_from_op(op);
    int ea_reg = ea_reg_from_op(op);
    /* Data alterable: Dn, (An), (An)+, -(An), d(An), (d8,An,Xn), abs.w, abs.l. Reject An, #imm, d(PC), (d8,PC,Xn). */
    if (ea_mode == 1 || (ea_mode == 7 && ea_reg >= 2 && ea_reg <= 4))
        return op_unimplemented(op);
    ea_store_value(ea_mode, ea_reg, 2, (uint32_t)cpu.sr & 0xFFFF);
    return move_cycles(0, 0, ea_mode, ea_reg, 2);
}

/* MOVE.W <ea>, CCR. 0x42C0-0x43FF. Source EA in bits 5-0. */
static int op_move_ccr(uint16_t op)
{
    int ea_mode = ea_mode_from_op(op);
    int ea_reg = ea_reg_from_op(op);
    uint16_t val = (uint16_t)(ea_fetch_value(ea_mode, ea_reg, 2) & 0xFFFF);
    cpu.sr = (cpu.sr & 0xFF00) | (val & 0xFF);
    return move_cycles(ea_mode, ea_reg, 0, 0, 2);
}

/* MOVE.W <ea>, SR. 0x46C0-0x47FF. Privileged. Source EA in bits 5-0. */
static int op_move_sr(uint16_t op)
{
    if (!require_supervisor())
        return 0;
    int ea_mode = ea_mode_from_op(op);
    int ea_reg = ea_reg_from_op(op);
    uint16_t val = (uint16_t)(ea_fetch_value(ea_mode, ea_reg, 2) & 0xFFFF);
    cpu.sr = val;
    return move_cycles(ea_mode, ea_reg, 0, 0, 2);
}

/* CLR <ea>. 0x4200, 0x4240, 0x4280. Store 0, set Z, clear N,V,C. An+byte illegal. */
static int op_clr(uint16_t op)
{
    int ea_mode = ea_mode_from_op(op);
    int ea_reg = ea_reg_from_op(op);
    int size = decode_size_bits_6_7(op);

    if (ea_reject_byte_an(ea_mode, size))
        return op_unimplemented(op);

    ea_store_value(ea_mode, ea_reg, size, 0);
    cpu.sr &= ~(SR_N | SR_V | SR_C);
    cpu.sr |= SR_Z;
    return clr_cycles(ea_mode, ea_reg, size);
}

#define CYCLES_EXT_SWAP  4
#define CYCLES_LINK      16
#define CYCLES_UNLK      12

/* EXT.W Dn: sign-extend byte to word. EXT.L Dn: sign-extend word to long. 0x4880-0x48FF. */
static int op_ext(uint16_t op)
{
    int dn = op & 7;
    int opmode = (op >> 6) & 3;
    uint32_t result;

    if (opmode == 2) {
        /* EXT.W: byte -> word, preserve high word of Dn */
        int8_t b = (int8_t)(cpu.d[dn] & 0xFF);
        result = (cpu.d[dn] & 0xFFFF0000u) | ((uint32_t)(int32_t)(int16_t)b & 0xFFFF);
    } else if (opmode == 3) {
        /* EXT.L: word -> long */
        int16_t w = (int16_t)(cpu.d[dn] & 0xFFFF);
        result = (uint32_t)(int32_t)w;
    } else {
        return op_unimplemented(op);
    }
    cpu.d[dn] = result;
    cpu.sr &= ~(SR_N | SR_Z | SR_V | SR_C);
    set_nz_from_val(result, opmode == 2 ? 2 : 4);
    return CYCLES_EXT_SWAP;
}

/* SWAP Dn: exchange upper and lower 16 bits. 0x4840-0x4847. */
static int op_swap(uint16_t op)
{
    int dn = op & 7;
    uint32_t val = cpu.d[dn];
    uint32_t result = ((val >> 16) & 0xFFFF) | ((val & 0xFFFF) << 16);
    cpu.d[dn] = result;
    cpu.sr &= ~(SR_N | SR_Z | SR_V | SR_C);
    set_nz_from_val(result, 4);
    return CYCLES_EXT_SWAP;
}

/* LINK An, #disp: push An, An=SP, SP+=disp. 0x4E50-0x4E57. Word displacement. */
static int op_link(uint16_t op)
{
    int an = op & 7;
    int32_t disp = (int16_t)fetch16();
    uint32_t sp = cpu_sp() - 4;
    mem_write32(sp, cpu.a[an]);
    cpu_sp_set(sp);
    cpu.a[an] = sp;
    cpu_sp_set(sp + disp);
    return CYCLES_LINK;
}

/* UNLK An: SP=An, An=pop. 0x4E58-0x4E5F. */
static int op_unlk(uint16_t op)
{
    int an = op & 7;
    uint32_t sp = cpu.a[an];
    cpu_sp_set(sp);
    cpu.a[an] = mem_read32(sp);
    cpu_sp_set(sp + 4);
    return CYCLES_UNLK;
}

/* 0x4xxx: RESET, STOP, TRAPV, LINK, UNLK, JSR, JMP, TRAP, RTE, RTS, NOP, CHK, LEA, EXT, SWAP, TST, CLR, NOT. */
int dispatch_4xxx(uint16_t op)
{
    if (op == 0x4E70) return op_reset(op);
    if (op == 0x4E72) return op_stop(op);
    if (op == 0x4E76) return op_trapv(op);
    /* ProcessorTests map: 0x4E60-0x4E67 = MOVEtoUSP (An->USP), 0x4E68-0x4E6F = MOVEfromUSP (USP->An) */
    if ((op & 0xFFF8) >= 0x4E60 && (op & 0xFFF8) <= 0x4E67) return op_move_an_to_usp(op);  /* MOVE An, USP */
    if ((op & 0xFFF8) >= 0x4E68 && (op & 0xFFF8) <= 0x4E6F) return op_move_usp_to_an(op);  /* MOVE USP, An */
    if ((op & 0xFFF8) == 0x4E50) return op_link(op);
    if ((op & 0xFFF8) == 0x4E58) return op_unlk(op);
    if ((op & 0xFFC0) == 0x4E80) return op_jsr(op);
    if ((op & 0xFFC0) == 0x4EC0) return op_jmp(op);
    if ((op & 0xFFF0) == 0x4E40) return op_trap(op);
    if (op == 0x4E73) return op_rte(op);
    if (op == 0x4E77) return op_rtr(op);
    if (op == 0x4E75) return op_rts(op);
    if (op == 0x4E71) return op_nop(op);
    if ((op & 0xFFC0) == 0x44C0 || (op & 0xFFC0) == 0x42C0) return op_move_ccr(op);  /* MOVE to CCR: 0x44C0 (ProcessorTests), 0x42C0 (Motorola) */
    if ((op & 0xFF00) == 0x4400) return op_neg(op);   /* NEG 0x4400-0x44BF */
    if ((op & 0xFF00) == 0x4000 && (op & 0x00C0) != 0x00C0) return op_negx(op);  /* NEGX 0x40xx, excl. MOVE from SR */
    if ((op & 0xFFC0) == 0x40C0) return op_move_from_sr(op);  /* MOVE from SR before CHK */
    if ((op & 0xF1C0) == 0x4180) return op_chk(op);  /* CHK before LEA */
    if ((op >> 8) >= 0x41 && (op >> 8) <= 0x4F && ((op >> 8) & 1)) return op_lea(op);  /* LEA */
    if (((op & 0xFFC0) == 0x4880 || (op & 0xFFC0) == 0x48C0) && movem_store_ea_valid((op >> 3) & 7, op & 7))
        return op_movem_store(op);   /* MOVEM.w 0x4880-0x48BF, MOVEM.l 0x48C0-0x48FF */
    if (((op & 0xFFC0) == 0x4C80 || (op & 0xFFC0) == 0x4CC0) && movem_load_ea_valid((op >> 3) & 7, op & 7))
        return op_movem_load(op);   /* MOVEM.w 0x4C80-0x4CBF, MOVEM.l 0x4CC0-0x4CFF */
    if ((op & 0xFF80) == 0x4880) return op_ext(op);
    if ((op & 0xFFF8) == 0x4840) return op_swap(op);   /* SWAP before PEA: 0x4840-0x4847 */
    if ((op & 0xFFC0) == 0x4840) return op_pea(op);    /* PEA: 0x4848-0x487F */
    if ((op & 0xFFC0) == 0x4800) return op_nbcd(op);
    if (op == 0x4AFC) return op_illegal(op);  /* ILLEGAL: explicit vector 4 */
    if ((op & 0xFFC0) == 0x4AC0) return op_tas(op);  /* TAS before TST */
    if ((op & 0xFF00) == 0x4A00) return op_tst(op);
    if ((op & 0xFF00) == 0x4200 && (op & 0x00C0) != 0x00C0) return op_clr(op);  /* CLR: 0x4200, 0x4240, 0x4280 */
    if ((op & 0xFFC0) == 0x46C0) return op_move_sr(op);   /* MOVE to SR before NOT */
    if ((op & 0xFFC0) == 0x4600 || (op & 0xFFC0) == 0x4640 || (op & 0xFFC0) == 0x4680)
        return op_not(op);   /* NOT.b, NOT.w, NOT.l */
    return op_unimplemented(op);
}

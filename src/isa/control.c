#include "cpu_internal.h"
#include "control.h"
#include "ea.h"
#include "logic.h"
#include "memory.h"
#include "movem.h"
#include "timing.h"

/* Fixed cycle counts for control/privileged instructions. */
#define CYCLES_STOP           4
#define CYCLES_RESET        132
#define CYCLES_TRAPV_NOT      4   /* TRAPV: no trap taken */
#define CYCLES_MOVE_USP       4   /* MOVE USP,An / MOVE An,USP */
#define CYCLES_RTR           20   /* RTR */
#define CYCLES_RTE           20   /* RTE */
#define CYCLES_RTD           16   /* RTD (68010+) */
#define CYCLES_MOVEC_TO_CR   10   /* MOVEC Rn, Rc  (write to control register) */
#define CYCLES_MOVEC_FROM_CR 12   /* MOVEC Rc, Rn  (read from control register) */
#define CYCLES_EXT_SWAP       4   /* EXT / SWAP */
#define CYCLES_LINK          16   /* LINK.W */
#define CYCLES_LINK_L        12   /* LINK.L (68020+) */
#define CYCLES_UNLK          12   /* UNLK */
#define CYCLES_TAS_DN         4   /* TAS Dn */
#define CYCLES_NOT_DN_BW      4   /* NOT Dn byte/word */
#define CYCLES_NOT_DN_L       6   /* NOT Dn long */
#define CYCLES_MOVE_FROM_SR_DN 6  /* MOVE SR, Dn */
#define CYCLES_MMU_WORD      28   /* MMU ops with 32-bit or no operand (PFLUSH, PMOVE 32-bit, PTEST) */
#define CYCLES_MMU_LONG      36   /* MMU ops with 64-bit operand (PMOVE SRP/CRP) */

/* MOVEC control register identifiers (extension word bits 11-0). */
#define CR_SFC  0x000  /* Source Function Code          (68010+) */
#define CR_DFC  0x001  /* Destination Function Code     (68010+) */
#define CR_CACR 0x002  /* Cache Control Register        (68020+) */
#define CR_TC   0x003  /* Translation Control           (68040+, via MOVEC) */
#define CR_ITT0 0x004  /* Instruction Transparent Translation 0 (68040+) */
#define CR_ITT1 0x005  /* Instruction Transparent Translation 1 (68040+) */
#define CR_DTT0 0x006  /* Data Transparent Translation 0        (68040+) */
#define CR_DTT1 0x007  /* Data Transparent Translation 1        (68040+) */
#define CR_BUSCR 0x008 /* Bus Control Register                  (68040+) */
#define CR_USP  0x800  /* User Stack Pointer            (68010+) */
#define CR_VBR  0x801  /* Vector Base Register          (68010+) */
#define CR_CAAR 0x802  /* Cache Address Register        (68020+) */
#define CR_MSP  0x803  /* Master Stack Pointer          (68020+) */
#define CR_ISP  0x804  /* Interrupt Stack Pointer       (68020+, maps to SSP) */
#define CR_MMUSR 0x805 /* MMU Status Register           (68040+, via MOVEC) */
#define CR_URP  0x806  /* User Root Pointer             (68040+) */
#define CR_SRP  0x807  /* Supervisor Root Pointer       (68040+, 32-bit via MOVEC) */
#define CR_PCR  0x808  /* Processor Control Register    (68040+) */

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
    if (!(imm & SR_S)) {  /* S-bit clear = user mode */
        cpu_take_exception(PRIVILEGE_VECTOR, 4);
        return 0;
    }
    cpu.sr = imm;
    cpu.halted = 1;
    return CYCLES_STOP;
}

/* TRAPV: 0x4E76. Trap on overflow (vector 7). Total trap = 34 cycles, no trap = 4. */
static int op_trapv(uint16_t op)
{
    (void)op;
    if (cpu.sr & SR_V) {
        cpu_take_exception(TRAPV_VECTOR, 0);
        return 0;
    }
    return CYCLES_TRAPV_NOT;
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
        cpu.sr &= ~(SR_N | SR_Z | SR_V | SR_C);
        if (dn_val < 0)
            cpu.sr |= SR_N;
        int chk_base = (dn_val > bound) ? 4 : 6;
        cpu_take_exception(CHK_VECTOR, ea_cycles(ea_mode, ea_reg, 2) + chk_base);
        return 0;
    }
    cpu.sr &= ~(SR_Z | SR_V | SR_C);  /* In bounds: N preserved (undefined/hardware-specific), Z/V/C cleared */
    return chk_cycles(ea_mode, ea_reg);
}

/* MOVE USP, An: 0x4E68-0x4E6F (ProcessorTests: MOVEfromUSP). Supervisor only. */
static int op_move_usp_to_an(uint16_t op)
{
    if (!require_supervisor())
        return 0;
    int an = op & 7;
    cpu.a[an] = cpu.usp;
    if (an == 7 && (cpu.sr & SR_S))
        cpu.ssp = cpu.a[7];  /* A7 = SSP in supervisor mode */
    return CYCLES_MOVE_USP;
}

/* MOVE An, USP: 0x4E60-0x4E67 (ProcessorTests: MOVEtoUSP). Supervisor only. */
static int op_move_an_to_usp(uint16_t op)
{
    if (!require_supervisor())
        return 0;
    int an = op & 7;
    cpu.usp = cpu.a[an];
    if (an == 7 && !(cpu.sr & SR_S))
        cpu.a[7] = cpu.usp;  /* User mode: A7 = USP */
    return CYCLES_MOVE_USP;
}

/* NOP: no operation. 0x4E71. */
static int op_nop(uint16_t op)
{
    (void)op;
    return CYCLES_NOP;
}

/* RTR: pop CCR (low byte of SR), then pop PC. 0x4E77.
 * Only the lower 5 bits (X,N,Z,V,C) are restored; bits 5-7 are unused. */
static int op_rtr(uint16_t op)
{
    uint32_t sp = cpu_sp();
    pending_cycles += 8;
    uint8_t ccr = (uint8_t)(mem_read16(sp) & 0xFF);
    pending_cycles += 4;
    cpu.sr = (cpu.sr & 0xFF00) | (ccr & 0x1F);
    cpu.pc = mem_read32(sp + 2);
    cpu_sp_set(sp + 6);
    if (cpu.pc & 1)
        cpu_take_addr_err(cpu.pc, op);
    return CYCLES_RTR;
}

/* RTS: pop return address from stack, jump to it. 0x4E75. */
static int op_rts(uint16_t op)
{
    uint32_t sp = cpu_sp();
    pending_cycles += 8;
    cpu.pc = mem_read32(sp);
    cpu_sp_set(sp + 4);
    if (cpu.pc & 1)
        cpu_take_addr_err(cpu.pc, op);
    return CYCLES_RTS;
}

/* MOVEC: move to/from control register. 68010+. Privileged.
 * 0x4E7A = MOVEC Rc, Rn  (control register -> general register)
 * 0x4E7B = MOVEC Rn, Rc  (general register -> control register)
 * Extension word: bit15=D/A (0=Dn,1=An), bits14-12=register, bits11-0=control reg.
 *
 * Control register numbers:
 *   0x000  SFC   Source Function Code          (68010+)
 *   0x001  DFC   Destination Function Code     (68010+)
 *   0x002  CACR  Cache Control Register        (68020+)
 *   0x800  USP   User Stack Pointer            (68010+)
 *   0x801  VBR   Vector Base Register          (68010+)
 *   0x802  CAAR  Cache Address Register        (68020+)
 *   0x803  MSP   Master Stack Pointer          (68020+)
 *   0x804  ISP   Interrupt Stack Pointer       (68020+, maps to SSP here) */
static int op_movec(uint16_t op)
{
    if (!cpu.features.has_movec)
        return op_unimplemented(op);
    if (!require_supervisor())
        return 0;
    uint16_t ext = fetch16();
    int da  = (ext >> 15) & 1;
    int reg = (ext >> 12) & 7;
    int cr  = ext & 0xFFF;

    if (op & 1) {
        /* MOVEC Rn, Rc (0x4E7B): general register -> control register */
        uint32_t val = da ? cpu.a[reg] : cpu.d[reg];
        switch (cr) {
        case CR_SFC:  cpu.sfc  = val & 7; break;
        case CR_DFC:  cpu.dfc  = val & 7; break;
        case CR_CACR: if (!cpu.features.has_full_ea) return op_unimplemented(op);
                      cpu.cacr = val;     break;
        case CR_USP:  cpu.usp  = val;     break;
        case CR_VBR:  cpu.vbr  = val;     break;
        case CR_CAAR: if (!cpu.features.has_full_ea) return op_unimplemented(op);
                      cpu.caar = val;     break;
        case CR_MSP:  if (!cpu.features.has_msp)     return op_unimplemented(op);
                      cpu.msp  = val;     break;
        case CR_ISP:   if (!cpu.features.has_msp)  return op_unimplemented(op);
                       cpu.ssp   = val;    break;
        case CR_TC:    if (!cpu.features.has_fpu)  return op_unimplemented(op);
                       cpu.tc    = val;    break;
        case CR_ITT0:  if (!cpu.features.has_fpu)  return op_unimplemented(op);
                       cpu.itt0  = val;    break;
        case CR_ITT1:  if (!cpu.features.has_fpu)  return op_unimplemented(op);
                       cpu.itt1  = val;    break;
        case CR_DTT0:  if (!cpu.features.has_fpu)  return op_unimplemented(op);
                       cpu.dtt0  = val;    break;
        case CR_DTT1:  if (!cpu.features.has_fpu)  return op_unimplemented(op);
                       cpu.dtt1  = val;    break;
        case CR_BUSCR: if (!cpu.features.has_fpu)  return op_unimplemented(op);
                       cpu.buscr = val;    break;
        case CR_MMUSR: if (!cpu.features.has_fpu)  return op_unimplemented(op);
                       cpu.mmusr = (uint16_t)(val & 0xFFFF); break;
        case CR_URP:   if (!cpu.features.has_fpu)  return op_unimplemented(op);
                       cpu.urp   = val;    break;
        case CR_SRP:   if (!cpu.features.has_fpu)  return op_unimplemented(op);
                       cpu.srp   = val;    break;  /* 68040: 32-bit; stored in low 32 of uint64 */
        case CR_PCR:   if (!cpu.features.has_fpu)  return op_unimplemented(op);
                       cpu.pcr   = val;    break;
        default: return op_unimplemented(op);
        }
        return CYCLES_MOVEC_TO_CR;
    } else {
        /* MOVEC Rc, Rn (0x4E7A): control register -> general register */
        uint32_t val;
        switch (cr) {
        case CR_SFC:  val = cpu.sfc;  break;
        case CR_DFC:  val = cpu.dfc;  break;
        case CR_CACR: if (!cpu.features.has_full_ea) return op_unimplemented(op);
                      val = cpu.cacr; break;
        case CR_USP:  val = cpu.usp;  break;
        case CR_VBR:  val = cpu.vbr;  break;
        case CR_CAAR: if (!cpu.features.has_full_ea) return op_unimplemented(op);
                      val = cpu.caar; break;
        case CR_MSP:  if (!cpu.features.has_msp)     return op_unimplemented(op);
                      val = cpu.msp;  break;
        case CR_ISP:   if (!cpu.features.has_msp)  return op_unimplemented(op);
                       val = cpu.ssp;    break;
        case CR_TC:    if (!cpu.features.has_fpu)  return op_unimplemented(op);
                       val = cpu.tc;     break;
        case CR_ITT0:  if (!cpu.features.has_fpu)  return op_unimplemented(op);
                       val = cpu.itt0;   break;
        case CR_ITT1:  if (!cpu.features.has_fpu)  return op_unimplemented(op);
                       val = cpu.itt1;   break;
        case CR_DTT0:  if (!cpu.features.has_fpu)  return op_unimplemented(op);
                       val = cpu.dtt0;   break;
        case CR_DTT1:  if (!cpu.features.has_fpu)  return op_unimplemented(op);
                       val = cpu.dtt1;   break;
        case CR_BUSCR: if (!cpu.features.has_fpu)  return op_unimplemented(op);
                       val = cpu.buscr;  break;
        case CR_MMUSR: if (!cpu.features.has_fpu)  return op_unimplemented(op);
                       val = cpu.mmusr;  break;
        case CR_URP:   if (!cpu.features.has_fpu)  return op_unimplemented(op);
                       val = cpu.urp;    break;
        case CR_SRP:   if (!cpu.features.has_fpu)  return op_unimplemented(op);
                       val = (uint32_t)cpu.srp; break;  /* 68040: lower 32 bits */
        case CR_PCR:   if (!cpu.features.has_fpu)  return op_unimplemented(op);
                       val = cpu.pcr;    break;
        default: return op_unimplemented(op);
        }
        if (da) { cpu.a[reg] = val; if (reg == 7) sync_a7_to_sp(); }
        else      cpu.d[reg] = val;
        return CYCLES_MOVEC_FROM_CR;
    }
}

/* RTD #disp: return and deallocate. 68010+.
 * PC = [SSP]; SSP += 4 + sign_extend(disp). */
static int op_rtd(uint16_t op)
{
    if (!cpu.features.has_movec)
        return op_unimplemented(op);
    int32_t disp = (int32_t)(int16_t)fetch16();
    uint32_t sp = cpu.ssp;
    pending_cycles += 8;
    cpu.pc = mem_read32(sp);
    cpu.ssp = sp + 4 + disp;
    cpu.a[7] = cpu.ssp;
    if (cpu.pc & 1)
        cpu_take_addr_err(cpu.pc, op);
    return CYCLES_RTD;
}

/* BKPT #n (0x4848-0x484F): breakpoint. 68010+.
 * Performs a breakpoint acknowledge bus cycle; in emulation, takes the
 * illegal instruction exception (vector 4). */
static int op_bkpt(uint16_t op)
{
    (void)op;
    cpu.pc -= 2;
    cpu_take_exception(ILLEGAL_VECTOR, 4);
    return 0;  /* unreachable */
}

/* TRAP #n: software interrupt. 0x4E40-0x4E4F. Vector 32+n. Total = 34 cycles. */
static int op_trap(uint16_t op)
{
    int n = op & 0x0F;
    cpu_take_exception(32 + n, 0);
    return 0;  /* unreachable */
}

/* RTE: return from exception. 0x4E73. Supervisor only.
 * 68000: pops SR (2 bytes) then PC (4 bytes) — 6-byte frame.
 * 68010+: pops format/vector word, PC, then SR — 8-byte format-0 frame.
 * SR mask: high byte 0xA7 (T1,S,I2,I1,I0), low byte 0x1F (X,N,Z,V,C). */
static int op_rte(uint16_t op)
{
    if (!require_supervisor())
        return 0;
    uint32_t sp = cpu.ssp;
    uint16_t sr;

    if (cpu.features.has_vbr) {
        /* 68010+ format-0 frame: format/vector word + PC + SR (8 bytes). */
        pending_cycles += 4;
        uint16_t fmt_word = mem_read16(sp);
        sp += 2;
        if ((fmt_word >> 12) != 0) {
            /* Unsupported frame format — take format error (vector 14). */
            cpu.ssp = sp - 2;
            cpu_take_exception(14, 0);
            return 0;
        }
        pending_cycles += 8;
        cpu.pc = mem_read32(sp);
        sp += 4;
        pending_cycles += 4;
        sr = mem_read16(sp);
        sp += 2;
    } else {
        /* 68000 frame: SR + PC (6 bytes). */
        pending_cycles += 8;
        sr = mem_read16(sp);
        pending_cycles += 4;
        cpu.pc = mem_read32(sp + 2);
        sp += 6;
    }

    cpu.sr = ((sr >> 8) & 0xA7) << 8 | (sr & 0x1F);
    cpu.ssp = sp;
    cpu.a[7] = (cpu.sr & SR_S) ? cpu.ssp : cpu.usp;
    if (cpu.pc & 1)
        cpu_take_addr_err(cpu.pc, op);
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

/* NOT <ea>: one's complement. 0x46xx. An not allowed.
 * Dn: 4 (b/w), 6 (L). Memory: 8 + ea (b/w), 12 + ea (L). */
static int op_not(uint16_t op)
{
    ea_decoded_t d;
    if (!decode_not(op, &d))
        return 0;

    ea_rmw_t rmw;
    uint32_t val = ea_read_rmw(d.ea_mode, d.ea_reg, d.size, &rmw) & d.mask;
    uint32_t result = (~val) & d.mask;
    ea_write_rmw(&rmw, result);
    set_nz_from_val(result, d.size);
    if (d.ea_mode == 0)
        return (d.size == 4) ? CYCLES_NOT_DN_L : CYCLES_NOT_DN_BW;
    return ((d.size == 4) ? 12 : 8) + ea_cycles(d.ea_mode, d.ea_reg, d.size);
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
    pending_cycles += 4;
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
    if (an_reg == 7)
        sync_a7_to_sp();
    return lea_cycles(ea_mode, ea_reg);
}

/* JMP <ea>. 0x4EC0-0x4EFF. Invalid: Dn, An, #imm. */
static int op_jmp(uint16_t op)
{
    uint32_t addr;
    int ea_mode, ea_reg;
    decode_ea_addr_jmp_jsr(op, &addr, &ea_mode, &ea_reg);
    if (addr & 1) {
        if (ea_mode == 5 || (ea_mode == 7 && (ea_reg == 0 || ea_reg == 2)))
            pending_cycles -= 2;
        else if (ea_mode == 7 && ea_reg == 1)
            pending_cycles -= 4;
        cpu_take_addr_err(addr, op);
    }
    cpu.pc = addr;
    return jmp_cycles(ea_mode, ea_reg);
}

/* JSR <ea>. 0x4E80-0x4EBF. Invalid: Dn, An, #imm. */
static int op_jsr(uint16_t op)
{
    uint32_t addr;
    int ea_mode, ea_reg;
    decode_ea_addr_jmp_jsr(op, &addr, &ea_mode, &ea_reg);
    if (addr & 1) {
        if (ea_mode == 5 || (ea_mode == 7 && (ea_reg == 0 || ea_reg == 2)))
            pending_cycles -= 2;
        else if (ea_mode == 7 && ea_reg == 1)
            pending_cycles -= 4;
        cpu_take_addr_err(addr, op);
    }
    cpu_trace_jsr(addr);
    uint32_t sp = cpu_sp() - 4;
    pending_cycles += 4;
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
    ea_rmw_t rmw;
    uint8_t val = (uint8_t)(ea_read_rmw(ea_mode, ea_reg, 1, &rmw) & 0xFF);
    cpu.sr &= ~(SR_N | SR_Z | SR_V | SR_C);
    if (val == 0)
        cpu.sr |= SR_Z;
    if (val & 0x80)
        cpu.sr |= SR_N;
    uint8_t result = val | 0x80;
    ea_write_rmw(&rmw, result);
    if (ea_mode == 0)
        return CYCLES_TAS_DN;
    return 10 + ea_cycles(ea_mode, ea_reg, 1);
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
    ea_rmw_t rmw;
    uint32_t dest = ea_read_rmw(d.ea_mode, d.ea_reg, d.size, &rmw) & d.mask;
    uint32_t result = (0 - dest) & d.mask;
    ea_write_rmw(&rmw, result);
    set_nzvc_sub_sized(result, 0, dest, d.size, 1);  /* SUB: X=C */
    if (d.ea_mode == 0)
        return (d.size == 4) ? 6 : 4;
    return ((d.size == 4) ? 12 : 8) + ea_cycles(d.ea_mode, d.ea_reg, d.size);
}

/* NEGX <ea>: dest = 0 - dest - X. 0x40xx (excl. 0x40C0-0x43FF MOVE from SR). Z: cleared if result nonzero, else unchanged. */
static int op_negx(uint16_t op)
{
    ea_decoded_t d;
    ea_decode_from_op(op, &d);
    if (ea_is_an(d.ea_mode))
        return op_unimplemented(op);
    uint8_t x_in = (cpu.sr & SR_X) ? 1 : 0;
    ea_rmw_t rmw;
    uint32_t dest = ea_read_rmw(d.ea_mode, d.ea_reg, d.size, &rmw) & d.mask;
    uint32_t result = (0 - dest - x_in) & d.mask;
    ea_write_rmw(&rmw, result);
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
    /* V: set if both dest and result have MSB set (signed overflow in 0 - dest - X). */
    {
        uint32_t msb = (uint32_t)1 << (d.size * 8 - 1);
        if ((dest & msb) && (result & msb)) cpu.sr |= SR_V;
    }
    if (d.ea_mode == 0)
        return (d.size == 4) ? 6 : 4;
    return ((d.size == 4) ? 12 : 8) + ea_cycles(d.ea_mode, d.ea_reg, d.size);
}

/* MOVE.W SR, <ea>. 0x40C0-0x43FF. Dest EA in bits 5-0. Data alterable only.
 * Unprivileged on 68000; privileged on 68010+. */
static int op_move_from_sr(uint16_t op)
{
    if (cpu.features.has_vbr && !require_supervisor())
        return 0;
    int ea_mode = ea_mode_from_op(op);
    int ea_reg = ea_reg_from_op(op);
    /* Data alterable: Dn, (An), (An)+, -(An), d(An), (d8,An,Xn), abs.w, abs.l. Reject An, #imm, d(PC), (d8,PC,Xn). */
    if (ea_mode == 1 || (ea_mode == 7 && ea_reg >= 2 && ea_reg <= 4))
        return op_unimplemented(op);
    /* 68000 performs a dummy read before writing (real hardware behavior). */
    ea_rmw_t rmw;
    ea_read_rmw(ea_mode, ea_reg, 2, &rmw);
    ea_write_rmw(&rmw, (uint32_t)cpu.sr & 0xFFFF);
    if (ea_mode == 0)
        return CYCLES_MOVE_FROM_SR_DN;
    return 8 + ea_cycles(ea_mode, ea_reg, 2);
}

/* MOVE.W <ea>, CCR. 0x42C0-0x43FF. Source EA in bits 5-0.
 * 68000: only lower 5 bits (X,N,Z,V,C) are implemented; mask to 0x1F. */
static int op_move_ccr(uint16_t op)
{
    int ea_mode = ea_mode_from_op(op);
    int ea_reg = ea_reg_from_op(op);
    uint16_t val = (uint16_t)(ea_fetch_value(ea_mode, ea_reg, 2) & 0xFFFF);
    cpu.sr = (cpu.sr & 0xFF00) | (val & 0x1F);
    return 12 + ea_cycles(ea_mode, ea_reg, 2);
}

/* MOVE.W <ea>, SR. 0x46C0-0x47FF. Privileged. Source EA in bits 5-0.
 * 68000: only implemented SR bits are written (same mask as RTE: 0xA7 high, 0x1F CCR). */
static int op_move_sr(uint16_t op)
{
    if (!require_supervisor())
        return 0;
    int ea_mode = ea_mode_from_op(op);
    int ea_reg = ea_reg_from_op(op);
    uint16_t val = (uint16_t)(ea_fetch_value(ea_mode, ea_reg, 2) & 0xFFFF);
    cpu.sr = ((val >> 8) & 0xA7) << 8 | (val & 0x1F);
    return 12 + ea_cycles(ea_mode, ea_reg, 2);
}

/* CLR <ea>. 0x4200, 0x4240, 0x4280. Store 0, set Z, clear N,V,C. An+byte illegal.
 * 68000 performs a dummy read before writing (read-modify-write cycle). */
static int op_clr(uint16_t op)
{
    int ea_mode = ea_mode_from_op(op);
    int ea_reg = ea_reg_from_op(op);
    int size = decode_size_bits_6_7(op);

    if (ea_reject_byte_an(ea_mode, size))
        return op_unimplemented(op);

    ea_rmw_t rmw;
    ea_read_rmw(ea_mode, ea_reg, size, &rmw);  /* dummy read (real 68000 behavior) */
    ea_write_rmw(&rmw, 0);
    cpu.sr &= ~(SR_N | SR_V | SR_C);
    cpu.sr |= SR_Z;
    return clr_cycles(ea_mode, ea_reg, size);
}

/* EXT.W / EXT.L / EXTB.L: sign-extend a data register.
 *   EXT.W  (0x4880-0x4887, opmode=2): byte  -> word  (high word of Dn preserved)
 *   EXT.L  (0x48C0-0x48C7, opmode=3, bit8=0): word  -> long
 *   EXTB.L (0x49C0-0x49C7, opmode=3, bit8=1): byte  -> long  (68020+, skips the word step) */
static int op_ext(uint16_t op)
{
    int dn      = op & 7;
    int opmode  = (op >> 6) & 3;
    uint32_t result;

    if (opmode == 2) {
        /* EXT.W: byte -> word, preserve high word of Dn */
        int8_t b = (int8_t)(cpu.d[dn] & 0xFF);
        result = (cpu.d[dn] & 0xFFFF0000u) | ((uint32_t)(int32_t)(int16_t)b & 0xFFFF);
    } else if (opmode == 3 && (op & 0x0100)) {
        /* EXTB.L (68020+): sign-extend the low byte directly to a full 32-bit long.
         * Bit 8 of the opcode distinguishes this from EXT.L (bit 8 = 0). */
        result = (uint32_t)(int32_t)(int8_t)(cpu.d[dn] & 0xFF);
    } else if (opmode == 3) {
        /* EXT.L: word -> long */
        result = (uint32_t)(int32_t)(int16_t)(cpu.d[dn] & 0xFFFF);
    } else {
        return op_unimplemented(op);
    }
    cpu.d[dn] = result;
    cpu.sr &= ~(SR_N | SR_Z | SR_V | SR_C);
    set_nz_from_val(opmode == 2 ? (result & 0xFFFF) : result, opmode == 2 ? 2 : 4);
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
    pending_cycles += 4;
    uint32_t sp = cpu_sp() - 4;
    cpu_sp_set(sp);
    pending_cycles += 4;
    mem_write32(sp, cpu.a[an]);
    cpu.a[an] = sp;
    cpu_sp_set(sp + disp);
    return CYCLES_LINK;
}

/* LINK.L An, #disp32: 68020+ long-displacement version. 0x4808-0x480F.
 * Identical to LINK.W except the displacement is a full 32-bit signed value
 * rather than a sign-extended 16-bit value.  Useful when a stack frame larger
 * than ±32 KB is required, which LINK.W cannot express. */
static int op_link_l(uint16_t op)
{
    int an = op & 7;
    int32_t disp = (int32_t)fetch32();
    pending_cycles += 8;    /* fault-point: past the long displacement fetch */
    uint32_t sp = cpu_sp() - 4;
    cpu_sp_set(sp);
    pending_cycles += 4;    /* fault-point: past the stack push */
    mem_write32(sp, cpu.a[an]);
    cpu.a[an] = sp;
    cpu_sp_set(sp + disp);
    return CYCLES_LINK_L;
}

/* UNLK An: SP=An, An=pop, SP=SP+4. 0x4E58-0x4E5F.
 * For UNLK A7: SP+4 step comes first, then An=pop overrides A7/SSP. */
static int op_unlk(uint16_t op)
{
    int an = op & 7;
    uint32_t sp = cpu.a[an];
    cpu_sp_set(sp + 4);
    pending_cycles += 8;
    cpu.a[an] = mem_read32(sp);
    if (an == 7)
        sync_a7_to_sp();
    return CYCLES_UNLK;
}

/* ---- 68030 MMU instructions (F-line, CpID=0, TYPE=0: 0xF000-0xF0FF) ----
 *
 * All 68030 MMU instructions share the same first-word format:
 *   1111 000 000 ea_mode ea_reg  (CpID=0, TYPE=0=cpGEN)
 *
 * The extension word identifies the specific operation via bits 15-13:
 *   001 (0x2xxx) — PFLUSH/PFLUSHA: invalidate TLB (no-op; no real TLB)
 *   010 (0x4xxx) — PMOVE <preg>, <ea>: read MMU register → memory
 *   011 (0x6xxx) — PMOVE <ea>, <preg>: write memory → MMU register
 *   100 (0x8xxx) — PTEST: test address translation (sets MMUSR=0)
 *   101 (0xAxxx) — PMOVE SRP, <ea>: read Supervisor Root Pointer (64-bit)
 *   110 (0xCxxx) — PMOVE <ea>, SRP: write Supervisor Root Pointer (64-bit)
 *   111 (0xExxx) — PMOVE CRP variants (direction in bit 9)
 *   000 (0x0xxx) — PMOVE TT0/TT1 (bit 12 selects TT1, bit 9 = direction)
 *
 * No address translation is implemented; these are shadow-register stubs that
 * allow 68030 boot firmware to configure the MMU without faulting.
 *
 * Encodings confirmed from MC68030 User's Manual disassembly examples:
 *   PMOVE TC,  (An) → F0nn 4000   PMOVE (An), TC → F0nn 6000
 *   PFLUSHA         → F000 2400
 */
static int op_mmu(uint16_t op)
{
    uint16_t ext    = fetch16();
    int ea_mode     = (op >> 3) & 7;
    int ea_reg      = op & 7;
    uint32_t addr   = 0;

    if (!require_supervisor())
        return 0;

    int op_type = (ext >> 13) & 7;  /* bits 15-13 of extension word */

    switch (op_type) {
    case 1: /* PFLUSH / PFLUSHA — flush TLB (no-op; no real TLB) */
        /* Variants with an EA operand: resolve to advance PC past ext words */
        if (ea_mode >= 2)
            ea_address_no_fetch(ea_mode, ea_reg, &addr);
        return CYCLES_MMU_WORD;

    case 2: /* PMOVE <preg>, <ea> — read MMU register → memory */
        /* TC is the only 32-bit register at this ext code; others treated identically */
        if (ea_resolve_addr(ea_mode, ea_reg, 4, &addr))
            mem_write32(addr, cpu.tc);
        return CYCLES_MMU_WORD;

    case 3: /* PMOVE <ea>, <preg> — write memory → MMU register */
        if (ea_resolve_addr(ea_mode, ea_reg, 4, &addr))
            cpu.tc = mem_read32(addr);
        return CYCLES_MMU_WORD;

    case 4: /* PTEST — probe address translation; set MMUSR = 0 (no fault) */
        cpu.mmusr = 0;
        return CYCLES_MMU_WORD;

    case 5: /* PMOVE SRP, <ea> — read Supervisor Root Pointer (64-bit) */
        if (ea_resolve_addr(ea_mode, ea_reg, 4, &addr)) {
            mem_write32(addr,     (uint32_t)(cpu.srp >> 32));
            mem_write32(addr + 4, (uint32_t)(cpu.srp & 0xFFFFFFFFu));
        }
        return CYCLES_MMU_LONG;

    case 6: /* PMOVE <ea>, SRP — write Supervisor Root Pointer (64-bit) */
        if (ea_resolve_addr(ea_mode, ea_reg, 4, &addr)) {
            uint64_t hi = mem_read32(addr);
            uint64_t lo = mem_read32(addr + 4);
            cpu.srp = (hi << 32) | lo;
        }
        return CYCLES_MMU_LONG;

    case 7: /* PMOVE CRP — direction determined by bit 9 of ext word */
        if (ext & 0x0200) {
            if (ea_resolve_addr(ea_mode, ea_reg, 4, &addr)) {
                uint64_t hi = mem_read32(addr);
                uint64_t lo = mem_read32(addr + 4);
                cpu.crp = (hi << 32) | lo;
            }
        } else {
            if (ea_resolve_addr(ea_mode, ea_reg, 4, &addr)) {
                mem_write32(addr,     (uint32_t)(cpu.crp >> 32));
                mem_write32(addr + 4, (uint32_t)(cpu.crp & 0xFFFFFFFFu));
            }
        }
        return CYCLES_MMU_LONG;

    case 0: /* PMOVE TT0/TT1 — bit 12=TT1, bit 9=direction */
        if (ext & 0x0200) {
            /* write: EA → TT register */
            if (ea_resolve_addr(ea_mode, ea_reg, 4, &addr)) {
                if (ext & 0x1000) cpu.tt1 = mem_read32(addr);
                else              cpu.tt0 = mem_read32(addr);
            }
        } else {
            /* read: TT register → EA */
            if (ea_resolve_addr(ea_mode, ea_reg, 4, &addr)) {
                mem_write32(addr, (ext & 0x1000) ? cpu.tt1 : cpu.tt0);
            }
        }
        return CYCLES_MMU_WORD;

    default:
        return op_unimplemented(op);
    }
}

int op_mmu_dispatch(uint16_t op)
{
    return op_mmu(op);
}

/* ---- 68040 cache control: CINV/CPUSH (0xF400-0xF4FF) ---- */

int op_cache_dispatch(uint16_t op)
{
    if (!require_supervisor())
        return 0;
    /* No cache is implemented; all CINVL/P/A and CPUSHL/P/A variants are no-ops.
     * The opcode encodes cache type (IC/DC/BC) and scope, but we ignore both. */
    (void)op;
    return 4;
}

/* ---- 68040 MOVE16: 16-byte aligned block transfer (0xF600-0xF6FF) ---- */

int op_move16(uint16_t op)
{
    if (!require_supervisor())
        return 0;
    int ax  = op & 7;
    int sub = (op >> 3) & 7;  /* bits 5-3 distinguish the five MOVE16 forms */

    if (sub == 4) {
        /* MOVE16 (Ax)+, (Ay)+ — both registers post-incremented */
        uint16_t ext = fetch16();
        int ay = (ext >> 12) & 7;
        uint32_t src = cpu.a[ax] & ~0xFu;
        uint32_t dst = cpu.a[ay] & ~0xFu;
        for (int i = 0; i < 16; i++)
            mem_write8(dst + i, mem_read8(src + i));
        cpu.a[ax] += 16;
        cpu.a[ay] += 16;
        return 18;
    }
    /* Absolute-address forms: one EA is a 32-bit absolute, the other is a register.
     * sub 0: (Ax)+, abs    sub 1: abs, (Ax)+    sub 2: (Ax), abs    sub 3: abs, (Ax) */
    uint32_t abs_addr = fetch32();
    uint32_t reg_addr = cpu.a[ax] & ~0xFu;
    uint32_t src = (sub <= 1) ? reg_addr        : (abs_addr & ~0xFu);
    uint32_t dst = (sub <= 1) ? (abs_addr & ~0xFu) : reg_addr;
    for (int i = 0; i < 16; i++)
        mem_write8(dst + i, mem_read8(src + i));
    if (sub == 0 || sub == 1) cpu.a[ax] += 16;  /* post-increment variants */
    return 18;
}

/* ---- 68040 FPU stubs (0xF200-0xF3FF) ---- */

/* Map FPU source format specifier (bits 14-12 of ext word) to integer byte size. */
static int fpu_src_size(int fmt)
{
    switch (fmt) {
    case 6: return 1;   /* byte */
    case 4: return 2;   /* word */
    case 0: return 4;   /* long */
    case 1: return 4;   /* single (treat as 32-bit load for stub) */
    case 5: return 4;   /* double (load low 32 bits only for stub) */
    case 2: return 4;   /* extended (load low 32 bits only for stub) */
    default: return 4;
    }
}

/* Update FPSR condition code bits (N, Z, I, NaN in bits 27-24) for a register. */
static void fpu_update_fpsr(int fpn)
{
    uint32_t hi  = cpu.fp[fpn].mant_hi;
    uint32_t lo  = cpu.fp[fpn].mant_lo;
    uint32_t exp = cpu.fp[fpn].exp & 0x7FFF;
    cpu.fpsr &= ~0x0F000000u;
    if (exp == 0 && hi == 0 && lo == 0)
        cpu.fpsr |= (1u << 26);  /* Z (zero) */
    else if (cpu.fp[fpn].exp & 0x8000)
        cpu.fpsr |= (1u << 27);  /* N (negative) */
}

/* General FPU instruction handler (cpGEN, sub-type 0). */
static int op_fpu_gen(uint16_t op)
{
    uint16_t ext = fetch16();

    /* FMOVEM: bits 15-14 = 11 */
    if ((ext & 0xC000) == 0xC000) {
        /* FMOVEM stub: consume operand and return.  Proper FMOVEM would
         * move multiple FP registers to/from memory; for boot-level stubs
         * we just advance past any EA extension words and do nothing. */
        int ea_mode = (op >> 3) & 7;
        int ea_reg  = op & 7;
        uint32_t addr;
        ea_resolve_addr(ea_mode, ea_reg, 4, &addr);
        return 10;
    }

    /* Extension word layout (confirmed from MC68040 FPU manual):
     *   bit 14  : EA involved (1=EA op, 0=FP register only op)
     *   bit 13  : direction when bit14=1 (0=EA→FPn load, 1=FPn→EA store)
     *   bits12-10: format specifier (EA op) or source FP register (FP-only op)
     *   bits 9-7 : FP register (destination for load/FP-FP, source for store)
     *   bits 6-0 : opmode */
    int ea_op   = (ext >> 14) & 1;  /* 1 = EA is involved in the operation */
    int to_ea   = (ext >> 13) & 1;  /* 1 = FPn→EA (store), 0 = EA→FPn (load) */
    int src_fmt = (ext >> 10) & 7;  /* format (ea_op=1) or src FP reg (ea_op=0) */
    int fp_reg  = (ext >> 7)  & 7;  /* FP register number (bits 9-7) */
    int opmode  = ext & 0x7F;

    switch (opmode) {
    case 0x00: /* FMOVE */
        if (ea_op && to_ea) {
            /* FPn → EA (store): FP register content → memory */
            int ea_mode = (op >> 3) & 7;
            int ea_reg  = op & 7;
            int sz = fpu_src_size(src_fmt);
            ea_store_value(ea_mode, ea_reg, sz, cpu.fp[fp_reg].mant_lo);
        } else if (ea_op) {
            /* EA → FPn (load): memory → FP register */
            int ea_mode = (op >> 3) & 7;
            int ea_reg  = op & 7;
            int sz = fpu_src_size(src_fmt);
            uint32_t val = ea_fetch_value(ea_mode, ea_reg, sz);
            /* Sign-extend byte/word to 32 bits */
            if (sz == 1) val = (int32_t)(int8_t)val;
            else if (sz == 2) val = (int32_t)(int16_t)val;
            cpu.fp[fp_reg].mant_lo = val;
            cpu.fp[fp_reg].mant_hi = 0;
            cpu.fp[fp_reg].exp     = (val == 0) ? 0
                                   : ((val & 0x80000000u) ? 0x8001u : 0x0001u);
            fpu_update_fpsr(fp_reg);
        } else {
            /* FPm → FPn (src_fmt holds source FP register when ea_op=0) */
            cpu.fp[fp_reg] = cpu.fp[src_fmt];
            fpu_update_fpsr(fp_reg);
        }
        cpu.fpiar = cpu.pc - 4;
        return 10;

    case 0x18: /* FABS: absolute value — clear sign bit */
        if (!ea_op) {
            cpu.fp[fp_reg] = cpu.fp[src_fmt];
            cpu.fp[fp_reg].exp &= 0x7FFF;
            fpu_update_fpsr(fp_reg);
        }
        cpu.fpiar = cpu.pc - 4;
        return 6;

    case 0x1A: /* FNEG: negate — toggle sign bit */
        if (!ea_op) {
            cpu.fp[fp_reg] = cpu.fp[src_fmt];
            cpu.fp[fp_reg].exp ^= 0x8000;
            fpu_update_fpsr(fp_reg);
        }
        cpu.fpiar = cpu.pc - 4;
        return 6;

    default:
        /* All other FPU opcodes: NOP stub.  Set FPIAR so software can identify
         * the instruction, but do not take an exception (boot ROM may probe). */
        cpu.fpiar = cpu.pc - 4;
        return 6;
    }
}

/* FSAVE: save FPU context frame to memory (privileged). */
static int op_fsave(uint16_t op)
{
    if (!require_supervisor())
        return 0;
    int ea_mode = (op >> 3) & 7;
    int ea_reg  = op & 7;
    uint32_t addr;
    if (ea_resolve_addr(ea_mode, ea_reg, 4, &addr)) {
        /* Write a 4-byte null/idle state frame (format word 0x0000). */
        mem_write16(addr,     0x0000);
        mem_write16(addr + 2, 0x0000);
    }
    return 8;
}

/* FRESTORE: restore FPU context frame from memory (privileged). */
static int op_frestore(uint16_t op)
{
    if (!require_supervisor())
        return 0;
    int ea_mode = (op >> 3) & 7;
    int ea_reg  = op & 7;
    uint32_t addr;
    /* Read (and discard) the frame format word. */
    if (ea_resolve_addr(ea_mode, ea_reg, 4, &addr))
        (void)mem_read16(addr);
    return 8;
}

/* FPU branch / cpScc / cpDBcc stubs: consume displacement and do not branch. */
static int op_fpu_bcc(uint16_t op, int sub)
{
    (void)op;
    if (sub == 4) fetch16();   /* cpBcc.W: 16-bit displacement */
    if (sub == 5) fetch32();   /* cpBcc.L: 32-bit displacement */
    /* cpScc (sub 2) and cpDBcc (sub 1): no extra words beyond the extension word
     * already consumed by the caller; fall through is the correct stub behaviour. */
    return 4;
}

int op_fpu_dispatch(uint16_t op)
{
    /* Bits 8-6 of the first opcode word identify the FPU sub-type. */
    uint8_t sub = (op >> 6) & 7;
    switch (sub) {
    case 0: return op_fpu_gen(op);    /* cpGEN: general FP instruction */
    case 6: return op_fsave(op);      /* FSAVE */
    case 7: return op_frestore(op);   /* FRESTORE */
    default: return op_fpu_bcc(op, sub);  /* cpBcc, cpDBcc, cpScc, cpTRAPcc */
    }
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
    if (op == 0x4E74) return op_rtd(op);        /* RTD (68010+) */
    if (op == 0x4E7A || op == 0x4E7B) return op_movec(op);  /* MOVEC (68010+) */
    if (op == 0x4E71) return op_nop(op);
    if ((op & 0xFFC0) == 0x44C0 || (op & 0xFFC0) == 0x42C0) return op_move_ccr(op);  /* MOVE to CCR: 0x44C0 (ProcessorTests), 0x42C0 (Motorola) */
    if ((op & 0xFF00) == 0x4400) return op_neg(op);   /* NEG 0x4400-0x44BF */
    if ((op & 0xFF00) == 0x4000 && (op & 0x00C0) != 0x00C0) return op_negx(op);  /* NEGX 0x40xx, excl. MOVE from SR */
    if ((op & 0xFFC0) == 0x40C0) return op_move_from_sr(op);  /* MOVE from SR before CHK */
    if ((op & 0xF1C0) == 0x4180) return op_chk(op);  /* CHK before LEA */
    if ((op & 0xFFF8) == 0x49C0 && cpu.features.has_full_ea) return op_ext(op); /* EXTB.L (68020+): must precede LEA */
    if ((op >> 8) >= 0x41 && (op >> 8) <= 0x4F && ((op >> 8) & 1)) return op_lea(op);  /* LEA */
    if (((op & 0xFFC0) == 0x4880 || (op & 0xFFC0) == 0x48C0) && movem_store_ea_valid((op >> 3) & 7, op & 7))
        return op_movem_store(op);   /* MOVEM.w 0x4880-0x48BF, MOVEM.l 0x48C0-0x48FF */
    if ((op & 0xFF80) == 0x4C00 && cpu.features.has_32bit_muldiv)
        return op_muldivl(op);      /* MULU.L/MULS.L 0x4C00-0x4C3F, DIVU.L/DIVS.L 0x4C40-0x4C7F (68020+) */
    if (((op & 0xFFC0) == 0x4C80 || (op & 0xFFC0) == 0x4CC0) && movem_load_ea_valid((op >> 3) & 7, op & 7))
        return op_movem_load(op);   /* MOVEM.w 0x4C80-0x4CBF, MOVEM.l 0x4CC0-0x4CFF */
    if ((op & 0xFF80) == 0x4880) return op_ext(op);
    if ((op & 0xFFF8) == 0x4840) return op_swap(op);   /* SWAP: 0x4840-0x4847 */
    if ((op & 0xFFF8) == 0x4808 && cpu.features.has_full_ea) return op_link_l(op); /* LINK.L (68020+) */
    if ((op & 0xFFF8) == 0x4848 && cpu.features.has_vbr) return op_bkpt(op); /* BKPT #n (68010+) */
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

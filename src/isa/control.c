#include "cpu_internal.h"
#include "control.h"
#include "ea.h"
#include "logic.h"
#include "memory.h"
#include "movem.h"
#include "timing.h"

/* Fixed cycle counts for control/privileged instructions. */
#define CYCLES_STOP           4
#define CYCLES_LPSTOP         8   /* approximate; real 68060 enters variable low-power state */
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

/* RESET: 0x4E70. Privileged. Assert external RESET line.
 * Resets all external hardware (CIAs, custom chips, etc.) via the system
 * callback.  The CPU itself is NOT reset — registers and PC are unchanged. */
static int op_reset(uint16_t op)
{
    (void)op;
    if (!require_supervisor())
        return 0;
    cpu_fire_reset_cb();
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

/* LPSTOP #imm (0xF800 + 16-bit imm): Low-power stop. Privileged. 68060 only.
 * Loads SR from immediate and halts until an interrupt arrives.
 * Behaviorally identical to STOP in this emulator (no actual power state). */
int op_lpstop(uint16_t op)
{
    (void)op;
    if (!require_supervisor())
        return 0;
    uint16_t imm = fetch16();
    cpu.sr = imm & SR_VALID;
    cpu.halted = 1;
    return CYCLES_LPSTOP;
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

/* =========================================================================
 * 68080 AMMX coprocessor (cpID=7, opcodes 0xFE00-0xFFFF)
 * =========================================================================
 *
 * Encoding (derived from vasm Apollo Core m68k backend, opcodes.h):
 *
 *   First opword:  1111 111 A B D | VEA
 *     bits 15-9  = 1111111 (F-line + cpID=7)
 *     bit  8 (A) = source-1 register bit 4 (extends D0-D7 to E8-E23)
 *     bit  7 (B) = source-2 register bit 4
 *     bit  6 (D) = destination  register bit 4
 *     bits 5-0   = VEA: source-1 EA
 *                    D0-D7  → 0x00-0x07 (mode=0, Dn)
 *                    E0-E7  → 0x08-0x0F (mode=1, An, reused as En for AMMX)
 *                    E8-E15 → 0x00-0x07 + A_bit=1
 *                    E16-E23→ 0x08-0x0F + A_bit=1
 *
 *   Extension word:
 *     bits 15-11 = destination register bits 3-0 (bit 4 from D-bit in first word)
 *     bits 10-6  = source-2   register bits 3-0 (bit 4 from B-bit in first word)
 *     bits  5-0  = opmode (6-bit instruction select)
 *
 *   Register mapping: 0-7 = D0-D7, 8-31 = E0-E23
 *   Read  D0-D7: zero-extended 32-bit; Write D0-D7: low 32 bits truncated.
 *
 * AMMX opmode table (from vasm Apollo Core opcodes.h):
 *   0x01 LOAD        0x02 TRANSHI     0x03 TRANSLO    0x04 STORE
 *   0x05 STOREM      0x06 PACKUSWB    0x07 PACK3216
 *   0x08 PAND        0x09 POR         0x0A PEOR        0x0B PANDN
 *   0x0C PAVGB
 *   0x10 PADDB       0x11 PADDW       0x12 PSUBB       0x13 PSUBW
 *   0x14 PADDUSB     0x15 PADDUSW     0x16 PSUBUSB     0x17 PSUBUSW
 *   0x18 PMUL88      0x19 PMULA       0x1A PMULH        0x1B PMULL
 *   0x1C BFLYB       0x1D BFLYW       0x1E UNPACK1632
 *   0x20 PCMPEQB     0x21 PCMPEQW    0x24 STOREC      0x25 STOREILM
 *   0x22 PCMPHIB     0x23 PCMPHIW    0x24 STOREC      0x25 STOREILM
 *   0x26 STOREM3     0x28 C2P         0x29 BSEL        0x2C PCMPGEB
 *   0x2D PCMPGEW     0x2E PCMPGTB    0x2F PCMPGTW
 *   0x30 PMINSB      0x31 PMINSW     0x32 PMINUB      0x33 PMINUW
 *   0x34 PMAXSB      0x35 PMAXSW     0x36 PMAXUB      0x37 PMAXUW
 *   0x38 LSLQ        0x39 LSRQ
 *   LOADI: same opmode as LOAD (0x01) but bit 12 of ext is set
 *   VPERM: special 3-word form, first word 0xFE3F; ext bits 4:0 = src1 reg;
 *          word 3 bits 4:0 = permutation-control reg
 */

#define CYCLES_AMMX  4   /* approximate; real 68080 timing varies per instruction */

/* Read a 64-bit AMMX register value.
 * reg 0-7  = D0-D7 (32-bit, zero-extended to 64)
 * reg 8-31 = E0-E23 (64-bit) */
static uint64_t ammx_reg_read(int reg)
{
    if (reg < 8) return (uint64_t)cpu.d[reg];
    return cpu.e[reg - 8];
}

/* Write a 64-bit AMMX register value.
 * D0-D7 receive the low 32 bits (truncated). */
static void ammx_reg_write(int reg, uint64_t val)
{
    if (reg < 8) cpu.d[reg] = (uint32_t)val;
    else cpu.e[reg - 8] = val;
}

/* Decode the source-1 register from a register-form AMMX first opword.
 * Returns a register number 0-31.  Only valid when VEA bits 5-4 indicate
 * a register operand (mode = 0 or 1); not called for memory EA forms. */
static int ammx_src1_reg(uint16_t op)
{
    int A_bit = (op >> 8) & 1;   /* extends register to E8-E23 range */
    int vea   = op & 0x0F;       /* bits 3-0: low 4 bits of register number */
    return (A_bit << 4) | vea;
}

/* Read 64 bits from a memory EA for AMMX LOAD.
 * Modes 3/(An)+ and 4/-(An) adjust An by 8 (64-bit stride).
 * All other memory modes use ea_resolve_addr (no An side-effect). */
static uint64_t ammx_ea_read64(int mode, int reg)
{
    uint32_t addr;
    if (mode == 2) {
        addr = cpu.a[reg];
    } else if (mode == 3) {
        addr = cpu.a[reg];
        cpu.a[reg] += 8;
    } else if (mode == 4) {
        cpu.a[reg] -= 8;
        addr = cpu.a[reg];
    } else {
        if (!ea_resolve_addr(mode, reg, 4, &addr))
            return 0;
    }
    return ((uint64_t)mem_read32(addr) << 32) | mem_read32(addr + 4);
}

/* Write 64 bits to a memory EA for AMMX STORE.
 * Same addressing conventions as ammx_ea_read64. */
static void ammx_ea_write64(int mode, int reg, uint64_t val)
{
    uint32_t addr;
    if (mode == 2) {
        addr = cpu.a[reg];
    } else if (mode == 3) {
        addr = cpu.a[reg];
        cpu.a[reg] += 8;
    } else if (mode == 4) {
        cpu.a[reg] -= 8;
        addr = cpu.a[reg];
    } else {
        if (!ea_resolve_addr(mode, reg, 4, &addr))
            return;
    }
    mem_write32(addr,     (uint32_t)(val >> 32));
    mem_write32(addr + 4, (uint32_t)val);
}

/* Packed byte add (8 × uint8 lanes). */
static uint64_t paddb(uint64_t a, uint64_t b)
{
    uint64_t result = 0;
    for (int i = 0; i < 8; i++) {
        uint8_t la = (uint8_t)(a >> (i * 8));
        uint8_t lb = (uint8_t)(b >> (i * 8));
        result |= (uint64_t)(uint8_t)(la + lb) << (i * 8);
    }
    return result;
}

/* Packed word add (4 × uint16 lanes). */
static uint64_t paddw(uint64_t a, uint64_t b)
{
    uint64_t result = 0;
    for (int i = 0; i < 4; i++) {
        uint16_t la = (uint16_t)(a >> (i * 16));
        uint16_t lb = (uint16_t)(b >> (i * 16));
        result |= (uint64_t)(uint16_t)(la + lb) << (i * 16);
    }
    return result;
}

/* Packed byte subtract. */
static uint64_t psubb(uint64_t a, uint64_t b)
{
    uint64_t result = 0;
    for (int i = 0; i < 8; i++) {
        uint8_t la = (uint8_t)(a >> (i * 8));
        uint8_t lb = (uint8_t)(b >> (i * 8));
        result |= (uint64_t)(uint8_t)(la - lb) << (i * 8);
    }
    return result;
}

/* Packed word subtract. */
static uint64_t psubw(uint64_t a, uint64_t b)
{
    uint64_t result = 0;
    for (int i = 0; i < 4; i++) {
        uint16_t la = (uint16_t)(a >> (i * 16));
        uint16_t lb = (uint16_t)(b >> (i * 16));
        result |= (uint64_t)(uint16_t)(la - lb) << (i * 16);
    }
    return result;
}

/* Packed unsigned byte add with saturation. */
static uint64_t paddusb(uint64_t a, uint64_t b)
{
    uint64_t result = 0;
    for (int i = 0; i < 8; i++) {
        uint32_t sum = (uint8_t)(a >> (i * 8)) + (uint8_t)(b >> (i * 8));
        result |= (uint64_t)(uint8_t)(sum > 0xFF ? 0xFF : sum) << (i * 8);
    }
    return result;
}

/* Packed unsigned word add with saturation. */
static uint64_t paddusw(uint64_t a, uint64_t b)
{
    uint64_t result = 0;
    for (int i = 0; i < 4; i++) {
        uint32_t sum = (uint16_t)(a >> (i * 16)) + (uint16_t)(b >> (i * 16));
        result |= (uint64_t)(uint16_t)(sum > 0xFFFF ? 0xFFFF : sum) << (i * 16);
    }
    return result;
}

/* Packed unsigned byte subtract with saturation. */
static uint64_t psubusb(uint64_t a, uint64_t b)
{
    uint64_t result = 0;
    for (int i = 0; i < 8; i++) {
        int32_t diff = (uint8_t)(a >> (i * 8)) - (uint8_t)(b >> (i * 8));
        result |= (uint64_t)(uint8_t)(diff < 0 ? 0 : diff) << (i * 8);
    }
    return result;
}

/* Packed unsigned word subtract with saturation. */
static uint64_t psubusw(uint64_t a, uint64_t b)
{
    uint64_t result = 0;
    for (int i = 0; i < 4; i++) {
        int32_t diff = (uint16_t)(a >> (i * 16)) - (uint16_t)(b >> (i * 16));
        result |= (uint64_t)(uint16_t)(diff < 0 ? 0 : diff) << (i * 16);
    }
    return result;
}

/* Packed unsigned byte average: (a + b + 1) >> 1. */
static uint64_t pavgb(uint64_t a, uint64_t b)
{
    uint64_t result = 0;
    for (int i = 0; i < 8; i++) {
        uint32_t avg = ((uint8_t)(a >> (i * 8)) + (uint8_t)(b >> (i * 8)) + 1) >> 1;
        result |= (uint64_t)(uint8_t)avg << (i * 8);
    }
    return result;
}

/* Packed byte compare-equal: lane = 0xFF if equal, else 0x00. */
static uint64_t pcmpeqb(uint64_t a, uint64_t b)
{
    uint64_t result = 0;
    for (int i = 0; i < 8; i++) {
        uint8_t mask = ((uint8_t)(a >> (i * 8)) == (uint8_t)(b >> (i * 8))) ? 0xFF : 0;
        result |= (uint64_t)mask << (i * 8);
    }
    return result;
}

/* Packed word compare-equal: lane = 0xFFFF if equal, else 0x0000. */
static uint64_t pcmpeqw(uint64_t a, uint64_t b)
{
    uint64_t result = 0;
    for (int i = 0; i < 4; i++) {
        uint16_t mask = ((uint16_t)(a >> (i * 16)) == (uint16_t)(b >> (i * 16))) ? 0xFFFF : 0;
        result |= (uint64_t)mask << (i * 16);
    }
    return result;
}

/* Packed signed byte compare-ge: lane = 0xFF if a[i] >= b[i] (signed).
 * AMMX convention: result = b >= a, so caller passes (b, a). */
static uint64_t pcmpgeb(uint64_t a, uint64_t b)
{
    uint64_t result = 0;
    for (int i = 0; i < 8; i++) {
        int8_t la = (int8_t)(uint8_t)(a >> (i * 8));
        int8_t lb = (int8_t)(uint8_t)(b >> (i * 8));
        result |= (uint64_t)(uint8_t)(la >= lb ? 0xFF : 0) << (i * 8);
    }
    return result;
}

/* Packed signed byte min. */
static uint64_t pminsb(uint64_t a, uint64_t b)
{
    uint64_t result = 0;
    for (int i = 0; i < 8; i++) {
        int8_t la = (int8_t)(uint8_t)(a >> (i * 8));
        int8_t lb = (int8_t)(uint8_t)(b >> (i * 8));
        result |= (uint64_t)(uint8_t)(la < lb ? la : lb) << (i * 8);
    }
    return result;
}

/* Packed signed word min. */
static uint64_t pminsw(uint64_t a, uint64_t b)
{
    uint64_t result = 0;
    for (int i = 0; i < 4; i++) {
        int16_t la = (int16_t)(uint16_t)(a >> (i * 16));
        int16_t lb = (int16_t)(uint16_t)(b >> (i * 16));
        result |= (uint64_t)(uint16_t)(la < lb ? la : lb) << (i * 16);
    }
    return result;
}

/* Packed unsigned byte min. */
static uint64_t pminub(uint64_t a, uint64_t b)
{
    uint64_t result = 0;
    for (int i = 0; i < 8; i++) {
        uint8_t la = (uint8_t)(a >> (i * 8));
        uint8_t lb = (uint8_t)(b >> (i * 8));
        result |= (uint64_t)(la < lb ? la : lb) << (i * 8);
    }
    return result;
}

/* Packed unsigned word min. */
static uint64_t pminuw(uint64_t a, uint64_t b)
{
    uint64_t result = 0;
    for (int i = 0; i < 4; i++) {
        uint16_t la = (uint16_t)(a >> (i * 16));
        uint16_t lb = (uint16_t)(b >> (i * 16));
        result |= (uint64_t)(la < lb ? la : lb) << (i * 16);
    }
    return result;
}

/* Packed signed byte max. */
static uint64_t pmaxsb(uint64_t a, uint64_t b)
{
    uint64_t result = 0;
    for (int i = 0; i < 8; i++) {
        int8_t la = (int8_t)(uint8_t)(a >> (i * 8));
        int8_t lb = (int8_t)(uint8_t)(b >> (i * 8));
        result |= (uint64_t)(uint8_t)(la > lb ? la : lb) << (i * 8);
    }
    return result;
}

/* Packed signed word max. */
static uint64_t pmaxsw(uint64_t a, uint64_t b)
{
    uint64_t result = 0;
    for (int i = 0; i < 4; i++) {
        int16_t la = (int16_t)(uint16_t)(a >> (i * 16));
        int16_t lb = (int16_t)(uint16_t)(b >> (i * 16));
        result |= (uint64_t)(uint16_t)(la > lb ? la : lb) << (i * 16);
    }
    return result;
}

/* Packed unsigned byte max. */
static uint64_t pmaxub(uint64_t a, uint64_t b)
{
    uint64_t result = 0;
    for (int i = 0; i < 8; i++) {
        uint8_t la = (uint8_t)(a >> (i * 8));
        uint8_t lb = (uint8_t)(b >> (i * 8));
        result |= (uint64_t)(la > lb ? la : lb) << (i * 8);
    }
    return result;
}

/* PMUL88: 8 × (uint8 × uint8 → uint16) packed into 4 × uint16 (low × high interleaved).
 * Each lane: result16 = (uint8(a) * uint8(b)) >> 8 — fractional 8×8 multiply. */
static uint64_t pmul88(uint64_t a, uint64_t b)
{
    uint64_t result = 0;
    for (int i = 0; i < 4; i++) {
        uint16_t la = (uint8_t)(a >> (i * 16));
        uint16_t lb = (uint8_t)(b >> (i * 16));
        result |= (uint64_t)(uint16_t)((la * lb) >> 8) << (i * 16);
    }
    return result;
}

/* PMULH: 4 × 16-bit lanes, keep upper 16 bits of signed 16×16 product. */
static uint64_t pmulh(uint64_t a, uint64_t b)
{
    uint64_t result = 0;
    for (int i = 0; i < 4; i++) {
        int32_t la = (int16_t)(a >> (i * 16));
        int32_t lb = (int16_t)(b >> (i * 16));
        result |= (uint64_t)(uint16_t)((la * lb) >> 16) << (i * 16);
    }
    return result;
}

/* PMULL: 4 × 16-bit lanes, keep lower 16 bits of signed 16×16 product. */
static uint64_t pmull(uint64_t a, uint64_t b)
{
    uint64_t result = 0;
    for (int i = 0; i < 4; i++) {
        int32_t la = (int16_t)(a >> (i * 16));
        int32_t lb = (int16_t)(b >> (i * 16));
        result |= (uint64_t)(uint16_t)(la * lb) << (i * 16);
    }
    return result;
}

/* PMULA: ARGB alpha-blend, 2 × 32-bit pixels per 64-bit register.
 * Per pixel: alpha = a[byte 0], if alpha == 255 → d = b (exact copy);
 * otherwise d[channel] = saturate8((alpha * b[channel]) >> 8 + a[channel]),
 * d[alpha byte] = 0.  (AC68080PRM page 65) */
static uint64_t pmula(uint64_t a, uint64_t b)
{
    uint64_t result = 0;
    for (int p = 0; p < 2; p++) {
        int base = p * 32;
        uint8_t alpha = (uint8_t)(a >> base);
        if (alpha == 255) {
            uint32_t pixel = (uint32_t)(b >> base) & 0x00FFFFFFu;
            result |= (uint64_t)pixel << base;
        } else {
            for (int c = 1; c <= 3; c++) {
                uint16_t ac = (uint8_t)(a >> (base + c * 8));
                uint16_t bc = (uint8_t)(b >> (base + c * 8));
                uint16_t val = ((alpha * bc) >> 8) + ac;
                result |= (uint64_t)(uint8_t)(val > 255 ? 255 : val) << (base + c * 8);
            }
        }
    }
    return result;
}

/* PMAXUW: 4 × uint16 lanes, unsigned word maximum. */
static uint64_t pmaxuw(uint64_t a, uint64_t b)
{
    uint64_t result = 0;
    for (int i = 0; i < 4; i++) {
        uint16_t la = (uint16_t)(a >> (i * 16));
        uint16_t lb = (uint16_t)(b >> (i * 16));
        result |= (uint64_t)(la > lb ? la : lb) << (i * 16);
    }
    return result;
}

/* PACKUSWB: pack 4 signed int16 lanes from a and 4 from b into 8 uint8 lanes.
 * Each lane is clamped to [0, 255] (unsigned saturation). */
static uint64_t packuswb(uint64_t a, uint64_t b)
{
    uint64_t result = 0;
    for (int i = 0; i < 4; i++) {
        int32_t la = (int16_t)(a >> (i * 16));
        uint8_t  ba = la < 0 ? 0 : la > 255 ? 255 : (uint8_t)la;
        result |= (uint64_t)ba << (i * 8);
    }
    for (int i = 0; i < 4; i++) {
        int32_t lb = (int16_t)(b >> (i * 16));
        uint8_t  bb = lb < 0 ? 0 : lb > 255 ? 255 : (uint8_t)lb;
        result |= (uint64_t)bb << ((i + 4) * 8);
    }
    return result;
}

/* PACK3216: pack 32-bit ARGB data from two registers into 4 × RGB565 words.
 * Each source register holds two ARGB pixels (4 bytes each, byte order A/R/G/B
 * from byte-index 0 upwards in little-endian lane convention).
 * Output: 4 RGB565 words in a single 64-bit register.
 * Equivalent C (from AMMX.doc.txt):
 *   if (i < 2) src = a  else src = b
 *   base = (i&1)*4
 *   diw = ((src[base+1]&0xF8)<<8) | ((src[base+2]&0xFC)<<3) | ((src[base+3]&0xF8)>>3) */
static uint64_t pack3216(uint64_t a, uint64_t b)
{
    uint64_t result = 0;
    for (int i = 0; i < 4; i++) {
        uint64_t src  = (i < 2) ? a : b;
        int      base = (i & 1) * 4;
        uint8_t  r    = (uint8_t)(src >> ((base + 1) * 8));
        uint8_t  g    = (uint8_t)(src >> ((base + 2) * 8));
        uint8_t  bv   = (uint8_t)(src >> ((base + 3) * 8));
        uint16_t diw  = (uint16_t)(((r & 0xF8u) << 8) | ((g & 0xFCu) << 3) | ((bv & 0xF8u) >> 3));
        result |= (uint64_t)diw << (i * 16);
    }
    return result;
}

/* UNPACK1632: expand 4 × RGB565 words into 4 × 32-bit ARGB pixels across a
 * consecutive register pair (dst and dst+1).
 * Alpha is filled as 0xFF; R/G/B channels are expanded with their low bits
 * replicated (standard RGB565→RGB888 expansion).
 * Equivalent C (from AMMX.doc.txt):
 *   d[i*4]   = 0xFF
 *   d[i*4+1] = ((a[i]>>8)&0xF8) | ((a[i]>>13)&0x7)
 *   d[i*4+2] = ((a[i]>>3)&0xFC) | ((a[i]>>9 )&0x3)
 *   d[i*4+3] = ((a[i]<<3)&0xF8) | ((a[i]>>2 )&0x7) */
static void op_ammx_unpack(uint64_t src, int dst)
{
    uint64_t out0 = 0, out1 = 0;
    for (int i = 0; i < 4; i++) {
        uint16_t w     = (uint16_t)(src >> (i * 16));
        uint8_t  alpha = 0xFF;
        uint8_t  red   = (uint8_t)(((w >> 8) & 0xF8u) | ((w >> 13) & 0x07u));
        uint8_t  green = (uint8_t)(((w >> 3) & 0xFCu) | ((w >>  9) & 0x03u));
        uint8_t  blue  = (uint8_t)(((w << 3) & 0xF8u) | ((w >>  2) & 0x07u));
        uint64_t pixel = (uint64_t)alpha
                       | ((uint64_t)red   <<  8)
                       | ((uint64_t)green << 16)
                       | ((uint64_t)blue  << 24);
        if (i < 2) out0 |= pixel << (i * 32);
        else       out1 |= pixel << ((i - 2) * 32);
    }
    ammx_reg_write(dst,     out0);
    ammx_reg_write(dst + 1, out1);
}

/* C2P: 8×8 bit-matrix transpose (chunky-to-planar).
 * Output byte i, bit (7-j) = input byte j, bit (7-i). */
static uint64_t c2p(uint64_t a)
{
    uint64_t result = 0;
    for (int i = 0; i < 8; i++) {
        uint8_t out = 0;
        for (int j = 0; j < 8; j++) {
            uint8_t sb = (uint8_t)(a >> (j * 8));
            if (sb & (uint8_t)(1u << (7 - i)))
                out |= (uint8_t)(1u << (7 - j));
        }
        result |= (uint64_t)out << (i * 8);
    }
    return result;
}


/* PCMPHIB: packed unsigned byte compare-high (unsigned GT). Lane = 0xFF or 0x00. */
static uint64_t pcmphib(uint64_t a, uint64_t b)
{
    uint64_t result = 0;
    for (int i = 0; i < 8; i++) {
        uint8_t la = (uint8_t)(a >> (i * 8));
        uint8_t lb = (uint8_t)(b >> (i * 8));
        result |= (uint64_t)(la > lb ? 0xFF : 0x00) << (i * 8);
    }
    return result;
}

/* PCMPHIW: packed unsigned word compare-high (unsigned GT). Lane = 0xFFFF or 0x0000. */
static uint64_t pcmphiw(uint64_t a, uint64_t b)
{
    uint64_t result = 0;
    for (int i = 0; i < 4; i++) {
        uint16_t la = (uint16_t)(a >> (i * 16));
        uint16_t lb = (uint16_t)(b >> (i * 16));
        result |= (uint64_t)(la > lb ? 0xFFFFU : 0x0000U) << (i * 16);
    }
    return result;
}

/* PCMPGTB: packed signed byte greater-than. Lane = 0xFF or 0x00. */
static uint64_t pcmpgtb(uint64_t a, uint64_t b)
{
    uint64_t result = 0;
    for (int i = 0; i < 8; i++) {
        int8_t la = (int8_t)(a >> (i * 8));
        int8_t lb = (int8_t)(b >> (i * 8));
        result |= (uint64_t)(la > lb ? 0xFF : 0x00) << (i * 8);
    }
    return result;
}

/* PCMPGEW: packed signed word greater-or-equal. Lane = 0xFFFF if a[i] >= b[i].
 * AMMX convention: result = b >= a, so caller passes (b, a). */
static uint64_t pcmpgew(uint64_t a, uint64_t b)
{
    uint64_t result = 0;
    for (int i = 0; i < 4; i++) {
        int16_t la = (int16_t)(a >> (i * 16));
        int16_t lb = (int16_t)(b >> (i * 16));
        result |= (uint64_t)(la >= lb ? 0xFFFFU : 0x0000U) << (i * 16);
    }
    return result;
}

/* PCMPGTW: packed signed word greater-than. Lane = 0xFFFF or 0x0000. */
static uint64_t pcmpgtw(uint64_t a, uint64_t b)
{
    uint64_t result = 0;
    for (int i = 0; i < 4; i++) {
        int16_t la = (int16_t)(a >> (i * 16));
        int16_t lb = (int16_t)(b >> (i * 16));
        result |= (uint64_t)(la > lb ? 0xFFFFU : 0x0000U) << (i * 16);
    }
    return result;
}

/* Core arithmetic compute: shared by register-form and memory-form paths.
 * Returns 1 and writes result to dst if opmode is recognised; returns 0 (no-op) otherwise. */
static int ammx_compute(int opmode, uint64_t a, uint64_t b, int dst)
{
    uint64_t result;
    switch (opmode) {
    /* Masked byte blend (reads existing dst; mask = low byte of b) */
    case 0x05: {                                                     /* STOREM  */
        uint8_t  mask   = (uint8_t)b;
        uint64_t d_orig = ammx_reg_read(dst);
        result = d_orig;
        for (int i = 0; i < 8; i++)
            if (mask & (uint8_t)(1u << (7 - i)))
                result = (result & ~(0xFFULL << (i * 8)))
                       | (a        &  (0xFFULL << (i * 8)));
        break;
    }

    /* Bitwise logical */
    case 0x08: result = a & b;                              break;  /* PAND    */
    case 0x09: result = a | b;                              break;  /* POR     */
    case 0x0A: result = a ^ b;                              break;  /* PEOR    */
    case 0x0B: result = (~a) & b;                           break;  /* PANDN   */

    /* Packed byte arithmetic (PSUB: D = B-A) */
    case 0x10: result = paddb(a, b);                        break;  /* PADDB   */
    case 0x12: result = psubb(b, a);                        break;  /* PSUBB   */
    case 0x14: result = paddusb(a, b);                      break;  /* PADDUSB */
    case 0x16: result = psubusb(b, a);                      break;  /* PSUBUSB */

    /* Packed word arithmetic (PSUB: D = B-A) */
    case 0x11: result = paddw(a, b);                        break;  /* PADDW   */
    case 0x13: result = psubw(b, a);                        break;  /* PSUBW   */
    case 0x15: result = paddusw(a, b);                      break;  /* PADDUSW */
    case 0x17: result = psubusw(b, a);                      break;  /* PSUBUSW */

    /* Average */
    case 0x0C: result = pavgb(a, b);                        break;  /* PAVGB   */

    /* Pack / unpack (UNPACK1632 writes register pair; handled outside ammx_compute) */
    case 0x06: result = packuswb(a, b);                     break;  /* PACKUSWB */
    case 0x07: result = pack3216(a, b);                     break;  /* PACK3216 */

    /* Multiply */
    case 0x18: result = pmul88(a, b);                       break;  /* PMUL88 */
    case 0x19: result = pmula(a, b);                       break;  /* PMULA  */
    case 0x1A: result = pmulh(a, b);                        break;  /* PMULH  */
    case 0x1B: result = pmull(a, b);                        break;  /* PMULL  */

    /* Compare */
    case 0x20: result = pcmpeqb(a, b);                      break;  /* PCMPEQB */
    case 0x21: result = pcmpeqw(a, b);                      break;  /* PCMPEQW */
    case 0x22: result = pcmphib(b, a);                      break;  /* PCMPHIB: b > a */
    case 0x23: result = pcmphiw(b, a);                      break;  /* PCMPHIW: b > a */

    /* Masked store variants (read-modify-write into dst) */
    case 0x24: {                                                     /* STOREC  */
        int count = (int)((uint8_t)b);
        if (count > 8) count = 8;
        uint64_t d_orig = ammx_reg_read(dst);
        result = d_orig;
        for (int i = 0; i < count; i++)
            result = (result & ~(0xFFULL << (i * 8)))
                   | (a        &  (0xFFULL << (i * 8)));
        break;
    }
    case 0x25: {                                                     /* STOREILM */
        uint64_t d_orig = ammx_reg_read(dst);
        result = d_orig;
        for (int i = 0; i < 8; i++)
            if (!((uint8_t)(b >> (i * 8)) & 0x80))
                result = (result & ~(0xFFULL << (i * 8)))
                       | (a        &  (0xFFULL << (i * 8)));
        break;
    }
    /* STOREM3 (0x26): handled outside ammx_compute — needs register number for mask_mode */

    case 0x28: result = c2p(a);                             break;  /* C2P    */
    case 0x29: result = (a & b) | (ammx_reg_read(dst) & ~b); break; /* BSEL   */
    case 0x2C: result = pcmpgeb(b, a);                      break;  /* PCMPGEB: b >= a */
    case 0x2D: result = pcmpgew(b, a);                      break;  /* PCMPGEW: b >= a */
    case 0x2E: result = pcmpgtb(b, a);                      break;  /* PCMPGTB: b > a  */
    case 0x2F: result = pcmpgtw(b, a);                      break;  /* PCMPGTW: b > a  */

    /* Min / max */
    case 0x30: result = pminsb(a, b);                       break;  /* PMINSB  */
    case 0x31: result = pminsw(a, b);                       break;  /* PMINSW  */
    case 0x32: result = pminub(a, b);                       break;  /* PMINUB  */
    case 0x33: result = pminuw(a, b);                       break;  /* PMINUW  */
    case 0x34: result = pmaxsb(a, b);                       break;  /* PMAXSB  */
    case 0x35: result = pmaxsw(a, b);                       break;  /* PMAXSW  */
    case 0x36: result = pmaxub(a, b);                       break;  /* PMAXUB  */
    case 0x37: result = pmaxuw(a, b);                       break;  /* PMAXUW  */

    /* 64-bit logical shifts: a = shift count, b = value to shift */
    case 0x38: result = b << (a & 63);                      break;  /* LSLQ   */
    case 0x39: result = b >> (a & 63);                      break;  /* LSRQ   */

    default: return 0;  /* unknown opmode — no-op, do not write */
    }
    ammx_reg_write(dst, result);
    return 1;
}

/* BFLYB/BFLYW: butterfly write to a consecutive register pair.
 * dst  receives the sum half; dst+1 receives the difference half.
 * Confirmed from vasm: VDR2/VXR2 = Dn:Dn+1 or En:En+1 double-register operand. */
static int op_ammx_bfly(int opmode, uint64_t a, uint64_t b, int dst)
{
    if (opmode == 0x1C) {           /* BFLYB: 8 × uint8 butterfly (64-bit register) */
        uint64_t sum = 0, diff = 0;
        for (int i = 0; i < 8; i++) {
            uint8_t la = (uint8_t)(a >> (i * 8));
            uint8_t lb = (uint8_t)(b >> (i * 8));
            sum  |= (uint64_t)(uint8_t)(la + lb) << (i * 8);
            diff |= (uint64_t)(uint8_t)(la - lb) << (i * 8);
        }
        ammx_reg_write(dst,     sum);
        ammx_reg_write(dst + 1, diff);
        return 1;
    }
    if (opmode == 0x1D) {           /* BFLYW: 4 × int16 butterfly (64-bit register) */
        uint64_t sum = 0, diff = 0;
        for (int i = 0; i < 4; i++) {
            int16_t la = (int16_t)(a >> (i * 16));
            int16_t lb = (int16_t)(b >> (i * 16));
            sum  |= (uint64_t)(uint16_t)(la + lb) << (i * 16);
            diff |= (uint64_t)(uint16_t)(la - lb) << (i * 16);
        }
        ammx_reg_write(dst,     sum);
        ammx_reg_write(dst + 1, diff);
        return 1;
    }
    return 0;
}

/* TRANSHI/TRANSLO: 4×4 word matrix transposition.
 * Reads 4 consecutive registers starting at src1 (must be multiple of 4).
 * Writes to consecutive pair dst, dst+1 (must be multiple of 2).
 *
 * TRANSHI (opmode 0x02): for each of the 4 input registers, extract words 0 and 1.
 *   dst[i]   = word0 of src1+i  (cols 0 from each row → first output)
 *   dst+1[i] = word1 of src1+i  (cols 1 from each row → second output)
 *
 * TRANSLO (opmode 0x03): same but words 2 and 3.
 *
 * Word k of a 64-bit register (little-endian lane convention): bits (k*16+15):(k*16).
 * Memory operands are not supported; this function is only called from register form. */
static int op_ammx_trans(int opmode, int src1, int dst)
{
    if (opmode != 0x02 && opmode != 0x03) return 0;
    int woff = (opmode == 0x02) ? 0 : 2;   /* TRANSHI: word pair 0,1; TRANSLO: 2,3 */
    uint64_t e = 0, f = 0;
    for (int i = 0; i < 4; i++) {
        uint64_t r  = ammx_reg_read(src1 + i);
        uint16_t we = (uint16_t)(r >> (woff       * 16));
        uint16_t wf = (uint16_t)(r >> ((woff + 1) * 16));
        e |= (uint64_t)we << (i * 16);
        f |= (uint64_t)wf << (i * 16);
    }
    ammx_reg_write(dst,     e);
    ammx_reg_write(dst + 1, f);
    return 1;
}

/* STOREM3: cookie-cut store with 4 mask modes (AC68080PRM page 73).
 * For each element of source data, a condition selects whether it is written.
 * mask_mode: 0=Long (msb=1), 1=Byte (!=0), 2=Word (!=0xF81F), 3=Word (msb=0).
 * Returns the merged result (src data where condition met, old dst where not). */
static uint64_t storem3(uint64_t src, uint64_t dst, int mask_mode)
{
    uint64_t result = dst;
    switch (mask_mode & 3) {
    case 0:
        for (int i = 0; i < 2; i++) {
            uint32_t lw = (uint32_t)(src >> (i * 32));
            if (lw & 0x80000000u)
                result = (result & ~(0xFFFFFFFFULL << (i * 32)))
                       | ((uint64_t)lw << (i * 32));
        }
        break;
    case 1:
        for (int i = 0; i < 8; i++) {
            uint8_t by = (uint8_t)(src >> (i * 8));
            if (by != 0)
                result = (result & ~(0xFFULL << (i * 8)))
                       | ((uint64_t)by << (i * 8));
        }
        break;
    case 2:
        for (int i = 0; i < 4; i++) {
            uint16_t w = (uint16_t)(src >> (i * 16));
            if (w != 0xF81F)
                result = (result & ~(0xFFFFULL << (i * 16)))
                       | ((uint64_t)w << (i * 16));
        }
        break;
    case 3:
        for (int i = 0; i < 4; i++) {
            uint16_t w = (uint16_t)(src >> (i * 16));
            if (!(w & 0x8000u))
                result = (result & ~(0xFFFFULL << (i * 16)))
                       | ((uint64_t)w << (i * 16));
        }
        break;
    }
    return result;
}

/* MINITERM: Amiga Blitter-style boolean raster-op on 3 operands.
 * 4 consecutive source registers provide: a (channel A), b (channel B),
 * c (channel C), mt (miniterm byte in low 8 bits).
 * For each of 64 bits, output = miniterm_bit[(a_bit<<2)|(b_bit<<1)|c_bit].
 * (AC68080PRM page 50) */
static uint64_t miniterm_op(uint64_t va, uint64_t vb, uint64_t vc, uint8_t mt)
{
    uint64_t result = 0;
    for (int i = 0; i < 64; i++) {
        int idx = (int)(((va >> i) & 1) << 2)
                | (int)(((vb >> i) & 1) << 1)
                | (int)((vc >> i) & 1);
        if (mt & (1 << idx))
            result |= 1ULL << i;
    }
    return result;
}


/* General AMMX instruction handler.
 * op  = first opword (0xFExx or 0xFFxx)
 * ext = extension word (already fetched by caller) */
static int op_ammx_gen(uint16_t op, uint16_t ext)
{
    /* Extension word layout:
     *   bits 15-11: destination register bits 3-0 (5th bit from D-bit in first word)
     *   bits 10-6 : source-2   register bits 3-0 (5th bit from B-bit in first word)
     *   bits  5-0 : opmode (6-bit instruction select)
     */
    int B_bit  = (op >> 7) & 1;
    int D_bit  = (op >> 6) & 1;

    int opmode = ext & 0x3F;
    int src2   = (B_bit << 4) | ((ext >> 6) & 0x0F);
    int dst    = (D_bit << 4) | ((ext >> 11) & 0x0F);

    /* VPERM: special 3-word byte permutation (VEA == 0x3F sentinel).
     * Word 2 (ext): dst[14:11] | src2[10:6] | src1[4:0]  (opmode field reused as src1)
     * Word 3      : permutation-control register number in bits [4:0]
     *
     * Output byte i = source byte at ctrl[i]&0xF:
     *   index 0-7  → src1 byte   index
     *   index 8-15 → src2 byte  (index-8)
     * NOTE: src1/src2 byte ordering unconfirmed — adjust if tests show reversal. */
    if ((op & 0x3F) == 0x3F) {
        uint16_t w3   = fetch16();
        int src1_r    = ext & 0x1F;          /* ext bits 4:0 = src1 register */
        int ctrl_r    = w3  & 0x1F;          /* word3 bits 4:0 = ctrl register */
        uint64_t va   = ammx_reg_read(src1_r);
        uint64_t vb   = ammx_reg_read(src2);
        uint64_t ctrl = ammx_reg_read(ctrl_r);
        uint64_t vr   = 0;
        for (int i = 0; i < 8; i++) {
            uint8_t idx  = (uint8_t)(ctrl >> (i * 8)) & 0x0F;
            uint8_t byte = (idx < 8) ? (uint8_t)(va >> (idx * 8))
                                     : (uint8_t)(vb >> ((idx - 8) * 8));
            vr |= (uint64_t)byte << (i * 8);
        }
        ammx_reg_write(dst, vr);
        return CYCLES_AMMX;
    }

    /* Determine whether this is a memory EA form or a register form.
     * VEA bits 5-3 (= op bits 5-3) encode the addressing mode:
     *   0 = Dn register direct
     *   1 = En register direct  (AMMX extended reg, treated as register)
     *   2+ = memory EA (indirect, post-inc, pre-dec, displacement, …) */
    int vea_mode = (op >> 3) & 7;
    int vea_reg  = op & 7;

    if (vea_mode >= 2) {
        /* Memory EA form. */
        switch (opmode) {
        case 0x01: /* LOAD (EA) → dst */
            ammx_reg_write(dst, ammx_ea_read64(vea_mode, vea_reg));
            break;
        case 0x04: /* STORE dst → (EA) */
            ammx_ea_write64(vea_mode, vea_reg, ammx_reg_read(dst));
            break;
        case 0x26: {
            /* STOREM3: cookie-cut store to memory.
             * dst = source data register ("b"), src2 & 3 = mask_mode.
             * Read existing memory, apply mask, write back. */
            uint64_t mem = ammx_ea_read64(vea_mode, vea_reg);
            uint64_t src = ammx_reg_read(dst);
            uint64_t res = storem3(src, mem, src2 & 3);
            ammx_ea_write64(vea_mode, vea_reg, res);
            break;
        }
        default: {
            uint64_t a = ammx_ea_read64(vea_mode, vea_reg);
            uint64_t b = ammx_reg_read(src2);
            if (!op_ammx_bfly(opmode, a, b, dst)) {
                if (opmode == 0x1E) op_ammx_unpack(a, dst);
                else                ammx_compute(opmode, a, b, dst);
            }
            break;
        }
        }
        return CYCLES_AMMX;
    }

    /* Register-form: both sources are registers. */
    int src1 = ammx_src1_reg(op);
    uint64_t a = ammx_reg_read(src1);
    uint64_t b = ammx_reg_read(src2);

    /* Register LOAD: copy src1 value to dst. */
    if (opmode == 0x01) {
        ammx_reg_write(dst, a);
        return CYCLES_AMMX;
    }
    /* Register STORE: copy src1 to dst. */
    if (opmode == 0x04) {
        ammx_reg_write(dst, a);
        return CYCLES_AMMX;
    }

    /* STOREM3 register form: no masking, just copy source to dst (PRM p73). */
    if (opmode == 0x26) {
        ammx_reg_write(dst, a);
        return CYCLES_AMMX;
    }

    /* MINITERM: 3-input boolean raster-op (PRM p50).
     * 4 consecutive source regs: a=channel A, b=channel B, c=channel C, mt=miniterm byte. */
    if (opmode == 0x2A) {
        uint64_t va = ammx_reg_read(src1);
        uint64_t vb = ammx_reg_read(src1 + 1);
        uint64_t vc = ammx_reg_read(src1 + 2);
        uint8_t  mt = (uint8_t)ammx_reg_read(src1 + 3);
        ammx_reg_write(dst, miniterm_op(va, vb, vc, mt));
        return CYCLES_AMMX;
    }

    /* Multi-register-write ops: BFLY (dst:dst+1 sum/diff), TRANS (4-reg transpose),
     * UNPACK1632 (RGB565→ARGB to dst:dst+1). */
    if (op_ammx_bfly(opmode, a, b, dst) ||
        op_ammx_trans(opmode, src1, dst))
        return CYCLES_AMMX;
    if (opmode == 0x1E) { op_ammx_unpack(a, dst); return CYCLES_AMMX; }
    ammx_compute(opmode, a, b, dst);
    return CYCLES_AMMX;
}

/* Top-level AMMX dispatch.
 * Unlike the standard Motorola coprocessor protocol, AMMX is integrated into
 * the 68080 pipeline — bits 8-6 of the first opword are NOT a sub-type selector.
 * They are the A/B/D register-extension flags already decoded by op_ammx_gen:
 *   bit 8 (A): extends src1 to the E8-E23 range
 *   bit 7 (B): extends src2 to the E8-E23 range
 *   bit 6 (D): extends dst  to the E8-E23 range
 * Treating them as cpSAVE/cpRESTORE/cpBcc selectors would misroute any
 * instruction whose operands lie in E8-E23, silently producing wrong results
 * or corrupting the stack (sub=6/7). cpSAVE and cpRESTORE stubs are kept as
 * dead helpers in case a future opcode study proves they exist. */
int op_ammx_dispatch(uint16_t op)
{
    uint16_t ext = fetch16();
    return op_ammx_gen(op, ext);
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

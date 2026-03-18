/*
 * 68000 instruction cycle counts. EA modes: 0=Dn, 1=An, 2=(An), 3=(An)+,
 * 4=-(An), 5=d(An), 6=(d8,An,Xn), 7=abs.w/abs.l/d(PC)/#imm.
 */

#include "timing.h"

/* EA calculation cycles: fetch ext words + compute + operand read. size: 1=B, 2=W, 4=L */
int ea_cycles(int mode, int reg, int size)
{
    int bw = (size == 4) ? 1 : 0;  /* 0 = byte/word, 1 = long */
    switch (mode) {
    case 0: case 1: return 0;
    case 2: case 3: return bw ? 8 : 4;
    case 4:          return bw ? 10 : 6;
    case 5:          return bw ? 12 : 8;
    case 6:          return bw ? 14 : 10;
    case 7:
        switch (reg) {
        case 0: return bw ? 12 : 8;   /* abs.w */
        case 1: return bw ? 16 : 12;  /* abs.l */
        case 2: return bw ? 12 : 8;   /* d(PC) */
        case 3: return bw ? 14 : 10;  /* (d8,PC,Xn) */
        case 4: return bw ? 8 : 4;    /* #imm */
        default: return 8;
        }
    default: return 0;
    }
}

/* Map mode+reg to table index 0-11. */
static int ea_index(int mode, int reg)
{
    if (mode <= 6) return mode;
    if (mode == 7) {
        if (reg == 0) return 7;
        if (reg == 1) return 8;
        if (reg == 2) return 9;
        if (reg == 3) return 10;
        if (reg == 4) return 11;
    }
    return 0;
}

/* MOVE byte/word: [dest][src]. From Motorola MOVE timing table. */
static const int move_bw[12][12] = {
    { 4, 4, 8, 8, 8, 12, 14, 12, 16, 12, 14, 8 },
    { 4, 4, 8, 8, 8, 12, 14, 12, 16, 12, 14, 8 },
    { 8, 8, 12, 12, 12, 16, 18, 16, 20, 16, 18, 12 },
    { 8, 8, 12, 12, 12, 16, 18, 16, 20, 16, 18, 12 },
    { 10, 10, 14, 14, 14, 18, 20, 18, 22, 18, 20, 14 },
    { 12, 12, 16, 16, 16, 20, 22, 20, 24, 20, 22, 16 },
    { 14, 14, 18, 18, 18, 22, 24, 22, 26, 22, 24, 18 },
    { 12, 12, 16, 16, 16, 20, 22, 20, 24, 20, 22, 16 },
    { 16, 16, 20, 20, 20, 24, 26, 24, 28, 24, 26, 20 },
    { 12, 12, 16, 16, 16, 20, 22, 20, 24, 20, 22, 16 },
    { 14, 14, 18, 18, 18, 22, 24, 22, 26, 22, 24, 18 },
    { 8, 8, 12, 12, 12, 16, 18, 16, 20, 16, 18, 12 },
};

/* MOVE long: [dest][src]. */
static const int move_l[12][12] = {
    { 4, 4, 12, 12, 12, 16, 18, 16, 20, 16, 18, 12 },
    { 4, 4, 12, 12, 12, 16, 18, 16, 20, 16, 18, 12 },
    { 12, 12, 20, 20, 20, 24, 26, 24, 28, 24, 26, 20 },
    { 12, 12, 20, 20, 20, 24, 26, 24, 28, 24, 26, 20 },
    { 14, 14, 22, 22, 22, 26, 28, 26, 30, 26, 28, 22 },
    { 16, 16, 24, 24, 24, 28, 30, 28, 32, 28, 30, 24 },
    { 18, 18, 26, 26, 26, 30, 32, 30, 34, 30, 32, 26 },
    { 16, 16, 24, 24, 24, 28, 30, 28, 32, 28, 30, 24 },
    { 20, 20, 28, 28, 28, 32, 34, 32, 36, 32, 34, 28 },
    { 16, 16, 24, 24, 24, 28, 30, 28, 32, 28, 30, 24 },
    { 18, 18, 26, 26, 26, 30, 32, 30, 34, 30, 32, 26 },
    { 12, 12, 20, 20, 20, 24, 26, 24, 28, 24, 26, 20 },
};

int move_cycles(int src_mode, int src_reg, int dst_mode, int dst_reg, int size)
{
    int di = ea_index(dst_mode, dst_reg);
    int si = ea_index(src_mode, src_reg);
    if (size == 4)
        return move_l[si][di];
    return move_bw[si][di];
}

/* MOVEP: 16 (word) or 24 (long). Motorola MC68000 d(An) form. */
int movep_cycles(int size)
{
    return (size == 4) ? 24 : 16;
}

/* ADD/SUB: base + EA. dir=0: <ea> to Dn, dir=1: Dn to <ea>. */
int add_sub_cycles(int ea_mode, int ea_reg, int size, int dir)
{
    int base;
    if (dir == 0) {
        base = (size == 4) ? 6 : 4;
        if (size == 4 && (ea_mode <= 1 || (ea_mode == 7 && ea_reg == 4)))
            base = 8;
    } else {
        base = (size == 4) ? 12 : 8;
    }
    return base + ea_cycles(ea_mode, ea_reg, size);
}

/* CMP: <ea> to Dn. Base 4 (B/W), 6 (L). No special Dn/An/#imm case. */
int cmp_cycles(int ea_mode, int ea_reg, int size)
{
    int base = (size == 4) ? 6 : 4;
    return base + ea_cycles(ea_mode, ea_reg, size);
}

/* ADDX/SUBX: Dy,Dx = 4(B/W) or 8(L); -(Ay),-(Ax) = 18(B/W) or 30(L). */
int addx_subx_cycles(int is_memory, int size)
{
    if (!is_memory)
        return (size == 4) ? 8 : 4;
    return (size == 4) ? 30 : 18;
}

/* PEA: 8 + LEA cycles (push + EA calc). */
int pea_cycles(int mode, int reg)
{
    return 8 + lea_cycles(mode, reg);
}

/* LEA: (An)=4, d(An)=8, (d8,An,Xn)=12, abs.w=8, abs.l=12, d(PC)=8, (d8,PC,Xn)=12 */
int lea_cycles(int mode, int reg)
{
    switch (mode) {
    case 2: return 4;
    case 5: return 8;
    case 6: return 12;
    case 7:
        switch (reg) {
        case 0: return 8;
        case 1: return 12;
        case 2: return 8;
        case 3: return 12;
        default: return 8;
        }
    default: return 4;
    }
}

/* JMP: (An)=8, d(An)=10, (d8,An,Xn)=14, abs.w=10, abs.l=12, d(PC)=10, (d8,PC,Xn)=14, (An)+/-(An)=10 */
int jmp_cycles(int mode, int reg)
{
    switch (mode) {
    case 2: return 8;
    case 3: case 4: return 10;
    case 5: return 10;
    case 6: return 14;
    case 7:
        switch (reg) {
        case 0: return 10;
        case 1: return 12;
        case 2: return 10;
        case 3: return 14;
        default: return 10;
        }
    default: return 8;
    }
}

/* JSR: (An)=16, d(An)=18, (d8,An,Xn)=22, abs.w=18, abs.l=20, d(PC)=18, (d8,PC,Xn)=22, (An)+/-(An)=18 */
int jsr_cycles(int mode, int reg)
{
    switch (mode) {
    case 2: return 16;
    case 3: case 4: return 18;
    case 5: return 18;
    case 6: return 22;
    case 7:
        switch (reg) {
        case 0: return 18;
        case 1: return 20;
        case 2: return 18;
        case 3: return 22;
        default: return 18;
        }
    default: return 16;
    }
}

/* Shift/rotate register: base 6 (B/W) or 8 (L) + 2 per count. Register count: 6+2n. */
int shift_cycles_register(int size, int count, int is_reg_count)
{
    int base = (size == 4) ? 8 : 6;
    if (is_reg_count)
        return base + 2 * count;
    return base + 2 * count;
}

/* Shift/rotate memory: 8 + ea_cycles (word). */
int shift_cycles_memory(int ea_mode, int ea_reg)
{
    return 8 + ea_cycles(ea_mode, ea_reg, 2);
}

/* MULU: 38 + 2*popcount(source) + ea. MULS: 38 + 2*f(source) + ea. */
int mulu_cycles(int ea_mode, int ea_reg, uint16_t source)
{
    return 38 + 2 * __builtin_popcount(source) + ea_cycles(ea_mode, ea_reg, 2);
}

int muls_cycles(int ea_mode, int ea_reg, uint16_t source)
{
    /* Count 0→1 and 1→0 transitions in 17-bit value <0, b15..b0> */
    uint32_t v = (uint32_t)source << 1;  /* shift left to compare with leading 0 */
    uint32_t transitions = v ^ (uint32_t)source;
    return 38 + 2 * __builtin_popcount(transitions & 0xFFFF) + ea_cycles(ea_mode, ea_reg, 2);
}

/*
 * DIVU cycle-accurate timing. Based on Jorge Cwik's (ijor) reverse-engineering
 * of the 68000 microcode restoring division algorithm.
 * Overflow: 10 cycles. Normal range: 76-136 cycles (register operand).
 */
int divu_cycles(int ea_mode, int ea_reg, uint32_t dividend, uint16_t divisor)
{
    if (divisor == 0)
        return 0;
    if ((dividend >> 16) >= (uint32_t)divisor)
        return 10 + ea_cycles(ea_mode, ea_reg, 2);

    unsigned mcycles = 38;
    uint32_t hdivisor = (uint32_t)divisor << 16;

    for (int i = 0; i < 15; i++) {
        uint32_t temp = dividend;
        dividend <<= 1;
        if ((int32_t)temp < 0) {
            dividend -= hdivisor;
        } else {
            mcycles += 2;
            if (dividend >= hdivisor) {
                dividend -= hdivisor;
                mcycles--;
            }
        }
    }
    return mcycles * 2 + ea_cycles(ea_mode, ea_reg, 2);
}

/*
 * DIVS cycle-accurate timing. Based on Jorge Cwik's (ijor) reverse-engineering
 * of the 68000 microcode signed division algorithm.
 * Absolute overflow: 16-18 cycles. Normal range: 120-156 cycles (register).
 */
int divs_cycles(int ea_mode, int ea_reg, int32_t dividend, int16_t divisor)
{
    if (divisor == 0)
        return 0;

    unsigned mcycles = 6;
    if (dividend < 0)
        mcycles++;

    int32_t quotient = dividend / (int32_t)divisor;
    if (quotient > 32767 || quotient < -32768)
        return (mcycles + 2) * 2 + ea_cycles(ea_mode, ea_reg, 2);

    uint32_t adividend = (uint32_t)((dividend < 0) ? -dividend : dividend);
    uint16_t adivisor = (uint16_t)((divisor < 0) ? -divisor : divisor);
    unsigned aquot = adividend / adivisor;
    mcycles += 55;

    if (divisor >= 0) {
        if (dividend >= 0)
            mcycles--;
        else
            mcycles++;
    }

    for (int i = 0; i < 15; i++) {
        if ((int16_t)aquot >= 0)
            mcycles++;
        aquot <<= 1;
    }

    return mcycles * 2 + ea_cycles(ea_mode, ea_reg, 2);
}

/* TST: Dn: 4. Memory: 4 + ea (B/W), 4 + ea (L). */
int tst_cycles(int mode, int reg, int size)
{
    if (mode <= 1)
        return 4;
    return 4 + ea_cycles(mode, reg, size);
}

/* Scc: Dn condition true = 6, Dn condition false = 4, Memory = 8 + ea. */
int scc_cycles_full(int ea_mode, int ea_reg, int cond_true)
{
    if (ea_mode == 0)
        return cond_true ? 6 : 4;
    return 8 + ea_cycles(ea_mode, ea_reg, 1);
}

/* DBcc: cc true = 12, branch taken = 10, count expired = 14 (Motorola MC68000). */
int dbcc_cycles(int state)
{
    switch (state) {
    case 0: return 12;  /* condition true: terminate */
    case 1: return 10;  /* condition false, count not expired: branch */
    case 2: return 14;  /* condition false, count expired: fall through */
    default: return 10;
    }
}

/* Scc: 4 for Dn, 8 + ea_cycles for memory (byte). */
int scc_cycles(int ea_mode, int ea_reg)
{
    if (ea_mode == 0)
        return 4;
    return 8 + ea_cycles(ea_mode, ea_reg, 1);
}

/* EXG: 6 cycles (Motorola). */
int exg_cycles(void)
{
    return 6;
}

/* ABCD/SBCD: 6 reg-reg, 18 mem-mem (Motorola). */
int abcd_sbcd_cycles(int is_memory)
{
    return is_memory ? 18 : 6;
}

/* NBCD: Dn = 6, memory = 8 + ea_cycles. */
int nbcd_cycles(int ea_mode, int ea_reg)
{
    if (ea_mode == 0)
        return 6;
    return 8 + ea_cycles(ea_mode, ea_reg, 1);
}

/* CHK: 10 + ea_cycles (word). Motorola MC68000. */
int chk_cycles(int ea_mode, int ea_reg)
{
    return 10 + ea_cycles(ea_mode, ea_reg, 2);
}

/* CLR: Dn: 4(B/W), 6(L). Memory: 8 + ea (B/W), 12 + ea (L). */
int clr_cycles(int mode, int reg, int size)
{
    if (mode == 0)
        return (size == 4) ? 6 : 4;
    return ((size == 4) ? 12 : 8) + ea_cycles(mode, reg, size);
}

/* Exception processing: stacking + vector fetch + first 2 words of handler. Motorola MC68000. */
int exception_cycles(int vector_num)
{
    switch (vector_num) {
    case 3:  return 50;  /* Address Error / Bus Error */
    case 4:  return 34;  /* Illegal Instruction / Trace */
    case 5:  return 34;  /* Divide by Zero */
    case 6:  return 34;  /* CHK (bounds violation) */
    case 7:  return 34;  /* TRAPV (trap taken) */
    case 8:  return 34;  /* Privilege Violation */
    case 9:  return 38;  /* Trace */
    case 32: case 33: case 34: case 35: case 36: case 37: case 38: case 39:
    case 40: case 41: case 42: case 43: case 44: case 45: case 46: case 47:
        return 34;  /* TRAP #0..#15 */
    case 10: return 42;  /* (A-Line) */
    case 11: return 42;  /* (F-Line) */
    case 14: return 44;  /* CHK (trap taken) */
    case 24: return 44;  /* Spurious interrupt */
    case 25: return 44;  /* Level 1-7 interrupt */
    default: return 34;  /* Fallback for unimplemented vectors */
    }
}

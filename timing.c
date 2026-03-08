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
        return move_l[di][si];
    return move_bw[di][si];
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

/* CMP: <ea> to Dn. */
int cmp_cycles(int ea_mode, int ea_reg, int size)
{
    int base = (size == 4) ? 6 : 4;
    if (size == 4 && (ea_mode <= 1 || (ea_mode == 7 && ea_reg == 4)))
        base = 8;
    return base + ea_cycles(ea_mode, ea_reg, size);
}

/* ADDX/SUBX: Dy,Dx = 4(B) or 8(W/L); -(Ay),-(Ax) = 18(B) or 30(L). */
int addx_subx_cycles(int is_memory, int size)
{
    if (!is_memory)
        return (size == 1) ? 4 : 8;
    return (size == 1) ? 18 : 30;
}

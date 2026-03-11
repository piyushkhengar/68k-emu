/*
 * 68000 instruction cycle counts. Based on Motorola MC68000 User Manual.
 * EA = effective address. Mode: 0=Dn, 1=An, 2=(An), 3=(An)+, 4=-(An),
 * 5=d(An), 6=(d8,An,Xn), 7=abs.w/abs.l/d(PC)/#imm.
 */

#ifndef TIMING_H
#define TIMING_H

/* Fixed-cycle instructions */
#define CYCLES_NOP      4
#define CYCLES_MOVEQ    4
#define CYCLES_RTS      12
#define CYCLES_BCC_NOT  8   /* Bcc when condition not met */
#define CYCLES_BCC_TAKEN 10 /* Bcc when condition met (byte disp) */
#define CYCLES_BSR      18

/* EA calculation cycles (fetch + compute + operand read). No write. */
int ea_cycles(int mode, int reg, int size);

/* MOVE cycles: full instruction time. size=1,2,4 for B,W,L. */
int move_cycles(int src_mode, int src_reg, int dst_mode, int dst_reg, int size);

/* ADD/SUB base + EA. dir=0: <ea> to Dn, dir=1: Dn to <ea>. */
int add_sub_cycles(int ea_mode, int ea_reg, int size, int dir);

/* CMP: <ea> to Dn only. */
int cmp_cycles(int ea_mode, int ea_reg, int size);

/* ADDX/SUBX: Dy,Dx = 4/8; -(Ay),-(Ax) = 18/30 (B/L). */
int addx_subx_cycles(int is_memory, int size);

/* Exception processing cycles (stacking + vector fetch + handler prefetch). Motorola MC68000. */
int exception_cycles(int vector_num);

/* Shift/rotate: register (size 1/2/4, count, is_reg_count) and memory (ea_mode, ea_reg). */
int shift_cycles_register(int size, int count, int is_reg_count);
int shift_cycles_memory(int ea_mode, int ea_reg);

/* MUL/DIV: base + EA fetch. Word form only. */
int mul_cycles(int ea_mode, int ea_reg);
int div_cycles(int ea_mode, int ea_reg, int is_signed);

/* DBcc, Scc cycle counts. */
int dbcc_cycles(int taken);
int scc_cycles(int ea_mode, int ea_reg);

/* EXG, ABCD/SBCD, CHK cycle counts. */
int exg_cycles(void);
int abcd_sbcd_cycles(int is_memory);
int chk_cycles(int ea_mode, int ea_reg);

/* LEA, JMP, JSR, TST, CLR cycle counts. */
int lea_cycles(int mode, int reg);
int jmp_cycles(int mode, int reg);
int jsr_cycles(int mode, int reg);
int tst_cycles(int mode, int reg, int size);
int clr_cycles(int mode, int reg, int size);

#endif /* TIMING_H */

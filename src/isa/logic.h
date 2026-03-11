/*
 * AND, OR, EOR logical operations.
 */

#ifndef LOGIC_H
#define LOGIC_H

/* 0x8xxx: OR (exclude DIVU/DIVS). */
int dispatch_8xxx(uint16_t op);

/* 0xCxxx: AND (exclude MULU/MULS). */
int dispatch_Cxxx(uint16_t op);

/* EOR: called from dispatch_Bxxx when opmode 4-6. */
int op_eor(uint16_t op);

/* NBCD: called from dispatch_4xxx. 0x4800-0x483F. */
int op_nbcd(uint16_t op);

#endif /* LOGIC_H */

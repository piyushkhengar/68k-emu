/*
 * ADDI, SUBI, CMPI, ADDQ, SUBQ dispatch.
 */

#ifndef IMMEDIATE_H
#define IMMEDIATE_H

/* 0x0xxx: ADDI (0x06), SUBI (0x04), CMPI (0x0C). */
int dispatch_0xxx(uint16_t op);

/* 0x5xxx: ADDQ (0x50), SUBQ (0x51). */
int dispatch_5xxx(uint16_t op);

#endif /* IMMEDIATE_H */

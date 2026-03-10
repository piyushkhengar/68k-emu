/*
 * 68000 shift and rotate instructions: ASL, ASR, LSL, LSR, ROL, ROR, ROXL, ROXR.
 * Opcodes 0xE0xx-0xE3xx (ASL/ASR, LSL/LSR) and 0xE5xx (ROL/ROR, ROXL/ROXR).
 */

#ifndef SHIFT_H
#define SHIFT_H

#include <stdint.h>

/* 0xExxx: route to shift/rotate or ADD. Called from cpu.c when op >> 12 == 0xE. */
int dispatch_Exxx(uint16_t op);

#endif /* SHIFT_H */

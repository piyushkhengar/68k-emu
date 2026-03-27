#ifndef CONTROL_H
#define CONTROL_H

#include <stdint.h>

int dispatch_4xxx(uint16_t op);

/* 68030 MMU instruction dispatch (PMOVE, PFLUSH, PTEST).
 * Called from dispatch_Fxxx when cpu.features.has_mmu is set and
 * the opcode is in the 0xF000-0xF0FF range (CpID=0, cpGEN). */
int op_mmu_dispatch(uint16_t op);

#endif /* CONTROL_H */

#ifndef CONTROL_H
#define CONTROL_H

#include <stdint.h>

int dispatch_4xxx(uint16_t op);

/* 68030 MMU instruction dispatch (PMOVE, PFLUSH, PTEST).
 * Called from dispatch_Fxxx when cpu.features.has_pmove is set and
 * the opcode is in the 0xF000-0xF0FF range (CpID=0, cpGEN). */
int op_mmu_dispatch(uint16_t op);

/* 68040 FPU instruction dispatch (0xF200-0xF3FF). */
int op_fpu_dispatch(uint16_t op);

/* 68040 cache control: CINV/CPUSH (0xF400-0xF4FF). */
int op_cache_dispatch(uint16_t op);

/* 68040 MOVE16: 16-byte cache-efficient block transfer (0xF600-0xF6FF). */
int op_move16(uint16_t op);

/* 68060+: LPSTOP #imm — low-power privileged stop (0xF800 + extension word). */
int op_lpstop(uint16_t op);

/* 68080: AMMX coprocessor dispatch (cpID=7, 0xFE00-0xFFFF).
 * Called from dispatch_Fxxx when cpu.features.has_ammx is set and
 * the opcode is in the 0xFE00-0xFFFF range (cpID=7). */
int op_ammx_dispatch(uint16_t op);

#endif /* CONTROL_H */

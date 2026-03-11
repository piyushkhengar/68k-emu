#ifndef CPU_H
#define CPU_H

#include <stdint.h>

/* Status Register bits (SR is 16-bit, upper byte is CCR) */
#define SR_X  (1 << 4)   /* Extend */
#define SR_N  (1 << 3)   /* Negative */
#define SR_Z  (1 << 2)   /* Zero */
#define SR_V  (1 << 1)   /* Overflow */
#define SR_C  (1 << 0)   /* Carry */

typedef struct {
    /* Data registers D0-D7 */
    uint32_t d[8];
    /* Address registers A0-A7 (A7 = active stack; ssp/usp hold the two stacks) */
    uint32_t a[8];
    uint32_t ssp;   /* Supervisor stack pointer (A7 when S=1) */
    uint32_t usp;   /* User stack pointer (A7 when S=0) */
    /* Program Counter */
    uint32_t pc;
    /* Status Register (16-bit) */
    uint16_t sr;

    /* Execution state */
    uint32_t cycles;     /* Cycle counter (for future use) */
    int halted;          /* HALT instruction sets this */
} CPU;

extern CPU cpu;

void cpu_init(void);
void cpu_reset(void);

/* Execute one instruction. Returns cycles executed (0 if halted). */
int cpu_step(void);

#endif /* CPU_H */

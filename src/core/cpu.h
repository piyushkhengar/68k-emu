#ifndef CPU_H
#define CPU_H

#include <stdint.h>

/* Status Register bits (SR is 16-bit, upper byte is CCR) */
#define SR_X  (1 << 4)   /* Extend */
#define SR_N  (1 << 3)   /* Negative */
#define SR_Z  (1 << 2)   /* Zero */
#define SR_V  (1 << 1)   /* Overflow */
#define SR_C  (1 << 0)   /* Carry */

/* Status Register structural bits (upper byte; only accessible in supervisor mode) */
#define SR_T      0x8000u  /* Trace mode */
#define SR_S      0x2000u  /* Supervisor mode */
#define SR_I_MASK 0x0700u  /* Interrupt priority mask (bits 10-8) */
#define SR_VALID  0xA71Fu  /* Mask of all defined SR bits */

/* Supported CPU models in the 68k family. */
typedef enum {
    CPU_MODEL_68000 = 0,
    CPU_MODEL_68010,
    CPU_MODEL_68020,
    CPU_MODEL_68030,
    CPU_MODEL_68040,
    CPU_MODEL_68060,
} cpu_model_t;

/* Feature flags derived from the model at cpu_init() time.
 * All flags are 0 on a 68000; each model sets the flags it introduces. */
typedef struct {
    unsigned has_vbr          : 1;  /* VBR register; vectors at VBR+n*4 (68010+) */
    unsigned has_movec        : 1;  /* MOVEC, MOVES, RTD instructions (68010+) */
    unsigned has_full_ea      : 1;  /* Full extension word, scaled index (68020+) */
    unsigned has_32bit_addr   : 1;  /* 32-bit address bus (68020+) */
    unsigned has_bitfield     : 1;  /* BFxxx bit-field instructions (68020+) */
    unsigned has_32bit_muldiv : 1;  /* MULS.L/MULU.L/DIVSL/DIVUL (68020+) */
    unsigned has_trapcc       : 1;  /* TRAPcc, CHK2/CMP2, CAS/CAS2 (68020+) */
    unsigned has_msp          : 1;  /* Master stack pointer / M-bit in SR (68020+) */
    unsigned has_mmu          : 1;  /* On-chip MMU: PMOVE/PFLUSH/PTEST (68030+) */
    unsigned has_fpu          : 1;  /* On-chip FPU (68040+) */
} cpu_features_t;

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
    uint16_t ir;         /* Instruction register: current opcode (set in cpu_step) */

    /* Model and derived feature flags (set by cpu_init, read-only at runtime) */
    cpu_model_t    model;
    cpu_features_t features;

    /* Control registers — present in struct for all models; only valid/used on
     * the model that introduced them (guarded by the corresponding feature flag). */
    uint32_t vbr;   /* 68010+: Vector Base Register (0 on 68000) */
    uint32_t sfc;   /* 68010+: Source Function Code */
    uint32_t dfc;   /* 68010+: Destination Function Code */
    uint32_t cacr;  /* 68020+: Cache Control Register */
    uint32_t caar;  /* 68020+: Cache Address Register */
    uint32_t msp;   /* 68020+: Master Stack Pointer */
    /* 68030+ MMU shadow registers (no address translation implemented;
     * these allow software to write/read MMU config without crashing). */
    uint32_t tc;    /* 68030+: Translation Control */
    uint64_t crp;   /* 68030+: CPU Root Pointer (64-bit) */
    uint64_t srp;   /* 68030+: Supervisor Root Pointer (64-bit) */
    uint32_t tt0;   /* 68030+: Transparent Translation 0 */
    uint32_t tt1;   /* 68030+: Transparent Translation 1 */
    uint16_t mmusr; /* 68030+: MMU Status Register */
} CPU;

extern CPU cpu;

/* External interrupt priority level (0-7). Set by hardware (e.g. VDP).
 * 0 = no interrupt pending. Checked at the start of each cpu_step(). */
extern int cpu_ipl;

/* Initialize the CPU core for the given model.  Must be called once before
 * any other cpu_* function.  Pass CPU_MODEL_68000 for standard Genesis use. */
void cpu_init(cpu_model_t model);
void cpu_reset(void);

/* Execute one instruction. Returns cycles executed (0 if halted). */
int cpu_step(void);

/* Interrupt-acknowledge callback.  Called when the CPU takes an autovector
 * interrupt, with the interrupt level (1-7).  The system layer should use
 * this to clear the pending interrupt in whatever peripheral asserted it
 * (e.g. the VDP on a Genesis, the VIA on a Macintosh).
 * Set to NULL (the default) if no acknowledge handling is needed. */
void cpu_set_int_ack(void (*fn)(int level));

#endif /* CPU_H */

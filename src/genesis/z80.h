#ifndef GENESIS_Z80_H
#define GENESIS_Z80_H

#include <stdint.h>

/*
 * Minimal Z80 CPU emulator for the Genesis sound subsystem.
 *
 * Handles the most common opcodes needed by Genesis sound drivers
 * (GEMS, SMPS, EA drivers, etc.) to execute their init sequences
 * and signal readiness to the 68K via handshake bytes in Z80 RAM.
 *
 * Z80 memory map on the Genesis:
 *   $0000-$1FFF  Z80 RAM (8 KB)
 *   $2000-$3FFF  Z80 RAM mirror
 *   $4000-$5FFF  YM2612 registers (stubbed)
 *   $6000-$60FF  Bank register (68K ROM window control)
 *   $7F11        PSG (SN76489, stubbed)
 *   $8000-$FFFF  68K address bank window
 */

void z80_init(void);
void z80_reset(void);
void z80_release_reset(void);

/* Execute one Z80 instruction. Returns cycle count, or 0 if halted. */
int  z80_step(void);

int  z80_is_running(void);

/* 68K bus access to Z80 RAM (used by bus.c when 68K reads/writes $A00000). */
uint8_t z80_ram_read(uint16_t addr);
void    z80_ram_write(uint16_t addr, uint8_t val);

#endif /* GENESIS_Z80_H */

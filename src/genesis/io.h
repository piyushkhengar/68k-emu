#ifndef GENESIS_IO_H
#define GENESIS_IO_H

#include <stdint.h>

/*
 * Genesis I/O and system registers (0xA10000 - 0xA1FFFF).
 *
 * I/O chip registers (accent on odd addresses -- 8-bit chip on D0-D7):
 *   0xA10001  Version register (region, video mode, revision)
 *   0xA10003  Controller port 1 data
 *   0xA10005  Controller port 2 data
 *   0xA10007  Expansion port data
 *   0xA10009  Controller port 1 control (TH pin direction)
 *   0xA1000B  Controller port 2 control
 *   0xA1000D  Expansion port control
 *   0xA1000F-1F  Serial I/O (stubbed)
 *
 * System registers:
 *   0xA11100  Z80 bus request (read: bus granted; write: request/release)
 *   0xA11200  Z80 reset (write: assert/deassert)
 *   0xA14000  TMSS "SEGA" register (write: accept and ignore)
 */

void io_init(void);
void io_reset(void);

uint8_t io_read8(uint32_t addr);
void    io_write8(uint32_t addr, uint8_t val);

/* Button bits for io_set_pad().  Active-high: set bit = button pressed. */
#define PAD_UP     (1 << 0)
#define PAD_DOWN   (1 << 1)
#define PAD_LEFT   (1 << 2)
#define PAD_RIGHT  (1 << 3)
#define PAD_A      (1 << 4)
#define PAD_B      (1 << 5)
#define PAD_C      (1 << 6)
#define PAD_START  (1 << 7)

/* Set the button state for controller 0 or 1.
 * buttons is a bitmask of PAD_* flags (1 = pressed). */
void io_set_pad(int idx, uint8_t buttons);

#endif /* GENESIS_IO_H */

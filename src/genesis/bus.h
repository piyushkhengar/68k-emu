#ifndef GENESIS_BUS_H
#define GENESIS_BUS_H

#include <stdint.h>
#include <stddef.h>

/*
 * Genesis memory map (active after bus_init):
 *
 *   0x000000 - 0x3FFFFF   Cartridge ROM (up to 4 MB, read-only)
 *   0xA00000 - 0xA0FFFF   Z80 address space (stubbed)
 *   0xA10000 - 0xA1001F   I/O ports (stubbed)
 *   0xA11100 - 0xA11101   Z80 bus request (stubbed)
 *   0xA11200 - 0xA11201   Z80 reset (stubbed)
 *   0xC00000 - 0xC0000F   VDP ports (stubbed)
 *   0xE00000 - 0xFFFFFF   68K work RAM (64 KB, mirrored)
 *
 * Unmapped reads return 0xFF; unmapped writes are ignored.
 */

/* Initialize the Genesis bus with a cartridge ROM image.
 * Copies the ROM data internally. Call before cpu_reset(). */
int  bus_init(const uint8_t *rom_data, size_t rom_size);
void bus_reset(void);

/* Bus read/write — called by the memory layer's function pointers.
 * Addresses are already masked to 24 bits by the caller. */
uint8_t  bus_read8(uint32_t addr);
uint16_t bus_read16(uint32_t addr);
uint32_t bus_read32(uint32_t addr);

void bus_write8(uint32_t addr, uint8_t val);
void bus_write16(uint32_t addr, uint16_t val);
void bus_write32(uint32_t addr, uint32_t val);

#endif /* GENESIS_BUS_H */

#ifndef MEMORY_H
#define MEMORY_H

#include <stdint.h>
#include <stddef.h>

/* Bus interface: CPU reads/writes through these. */
uint8_t  mem_read8(uint32_t addr);
uint16_t mem_read16(uint32_t addr);
uint32_t mem_read32(uint32_t addr);

void mem_write8(uint32_t addr, uint8_t val);
void mem_write16(uint32_t addr, uint16_t val);
void mem_write32(uint32_t addr, uint32_t val);

/* Initialize memory (e.g. 16MB RAM for test ROM). Reset clears to zero. */
void mem_init(void);
void mem_reset(void);

/* Load ROM/data at address 0 (e.g. Klaus Dormann test suite). */
void mem_load_rom(const uint8_t *data, size_t size);

#endif /* MEMORY_H */

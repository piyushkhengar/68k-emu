#ifndef MEMORY_H
#define MEMORY_H

#include <stdint.h>
#include <stddef.h>

#define MEM_SIZE (16 * 1024 * 1024)  /* 16MB - enough for test ROMs */

/* Bus interface: CPU reads/writes through these.
 * Dispatch to either flat RAM (tests) or Genesis bus depending on mode. */
uint8_t  mem_read8(uint32_t addr);
uint16_t mem_read16(uint32_t addr);
uint32_t mem_read32(uint32_t addr);

void mem_write8(uint32_t addr, uint8_t val);
void mem_write16(uint32_t addr, uint16_t val);
void mem_write32(uint32_t addr, uint32_t val);

/* Initialize memory in flat mode (16 MB RAM for tests). */
void mem_init(void);
void mem_reset(void);

/* Load ROM/data at address 0 (flat mode only). */
void mem_load_rom(const uint8_t *data, size_t size);

/*
 * Bus mode: redirect all memory access through a custom bus.
 * Pass NULL to revert to flat RAM mode (default).
 */
typedef struct {
    uint8_t  (*read8)(uint32_t addr);
    uint16_t (*read16)(uint32_t addr);
    uint32_t (*read32)(uint32_t addr);
    void     (*write8)(uint32_t addr, uint8_t val);
    void     (*write16)(uint32_t addr, uint16_t val);
    void     (*write32)(uint32_t addr, uint32_t val);
} mem_bus_t;

void mem_set_bus(const mem_bus_t *bus);

#endif /* MEMORY_H */

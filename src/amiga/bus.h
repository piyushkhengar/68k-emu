#ifndef AMIGA_BUS_H
#define AMIGA_BUS_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "paula.h"
#include "cia.h"
#include "agnus.h"
#include "denise.h"

/* Chip singletons owned by bus.c; used by amiga.c for per-scanline ticking. */
extern paula_t  amiga_paula;
extern cia_t    amiga_cia_a;
extern cia_t    amiga_cia_b;
extern agnus_t  amiga_agnus;
extern denise_t amiga_denise;

/* Chip RAM accessor — used by amiga.c to pass chip_ram to denise_render_line. */
const uint8_t *amiga_bus_chip_ram(void);
uint32_t       amiga_bus_chip_ram_size(void);

/*
 * Amiga 500 address bus — Phase 1 (boot to display window).
 *
 * Memory map:
 *   0x000000–0x07FFFF  Chip RAM (512 KB); reads return ROM when OVL active
 *   0x080000–0x0FFFFF  Open bus (returns 0xFF)
 *   0xBFD000–0xBFDFFF  CIA-B (even bytes; Phase 1 stub returns 0xFF)
 *   0xBFE001–0xBFEFFF  CIA-A (odd bytes; Phase 1 stub returns 0xFF)
 *   0xC00000–0xC7FFFF  Slow RAM (512 KB trapdoor expansion)
 *   0xDFF000–0xDFFFFF  Custom chip registers (Phase 1 stub returns 0)
 *   0xF80000–0xFFFFFF  Kickstart ROM (read-only; writes silently ignored)
 *
 * OVL (Gary chip):
 *   Active at reset. ROM is mirrored at 0x000000 so the 68000 can fetch
 *   valid SSP + PC reset vectors. Kickstart deactivates OVL by writing
 *   bit 0 of CIA-A PRA (0xBFE001).
 *
 * All functions are prefixed amiga_ to avoid collisions with genesis/bus.c.
 */

int  amiga_bus_init(const uint8_t *rom_data, size_t rom_size);
void amiga_bus_reset(void);

/* Set the Gary OVL overlay flag. Called internally by CIA-A PRA write. */
void amiga_bus_set_ovl(bool active);

uint8_t  amiga_bus_read8(uint32_t addr);
uint16_t amiga_bus_read16(uint32_t addr);
uint32_t amiga_bus_read32(uint32_t addr);

void amiga_bus_write8(uint32_t addr, uint8_t val);
void amiga_bus_write16(uint32_t addr, uint16_t val);
void amiga_bus_write32(uint32_t addr, uint32_t val);

#endif /* AMIGA_BUS_H */

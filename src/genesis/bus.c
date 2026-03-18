/*
 * Genesis bus: address decoding for the 68000 side.
 *
 * Routes reads and writes to cartridge ROM, work RAM, VDP, I/O, or the
 * Z80 address space.  VDP and I/O are stubs that will be filled in by
 * later phases; for now they return open-bus values so the ROM doesn't
 * crash on first contact.
 */

#include "bus.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ------------------------------------------------------------------ */
/*  Cartridge ROM                                                      */
/* ------------------------------------------------------------------ */

static uint8_t *cart_rom;
static uint32_t cart_rom_size;

/* ------------------------------------------------------------------ */
/*  68K work RAM: 64 KB at 0xE00000, mirrored through 0xFFFFFF        */
/* ------------------------------------------------------------------ */

#define WRAM_SIZE  0x10000
static uint8_t work_ram[WRAM_SIZE];

/* ------------------------------------------------------------------ */
/*  Init / Reset                                                       */
/* ------------------------------------------------------------------ */

int bus_init(const uint8_t *rom_data, size_t rom_size)
{
    free(cart_rom);
    cart_rom = NULL;
    cart_rom_size = 0;

    if (!rom_data || rom_size == 0) {
        fprintf(stderr, "bus_init: no ROM data\n");
        return -1;
    }

    cart_rom = (uint8_t *)malloc(rom_size);
    if (!cart_rom) {
        fprintf(stderr, "bus_init: out of memory\n");
        return -1;
    }
    memcpy(cart_rom, rom_data, rom_size);
    cart_rom_size = (uint32_t)rom_size;

    memset(work_ram, 0, WRAM_SIZE);
    return 0;
}

void bus_reset(void)
{
    memset(work_ram, 0, WRAM_SIZE);
}

/* ------------------------------------------------------------------ */
/*  8-bit read                                                         */
/* ------------------------------------------------------------------ */

uint8_t bus_read8(uint32_t addr)
{
    /* Cartridge ROM: 0x000000 - 0x3FFFFF */
    if (addr < 0x400000) {
        if (addr < cart_rom_size)
            return cart_rom[addr];
        return 0xFF;
    }

    /* Z80 address space: 0xA00000 - 0xA0FFFF (stub) */
    if (addr >= 0xA00000 && addr <= 0xA0FFFF)
        return 0xFF;

    /* I/O area: 0xA10000 - 0xA1001F (stub) */
    if (addr >= 0xA10000 && addr <= 0xA1001F)
        return 0xFF;

    /* Z80 bus request: 0xA11100 */
    if (addr == 0xA11100)
        return 0x01;  /* bus granted */
    if (addr == 0xA11101)
        return 0x00;

    /* Z80 reset: 0xA11200 */
    if (addr >= 0xA11200 && addr <= 0xA11201)
        return 0x00;

    /* VDP: 0xC00000 - 0xC0000F (stub — returns 0 for data, vblank for status) */
    if (addr >= 0xC00000 && addr <= 0xC0000F) {
        /* Status register at control port (C00004/C00005): return vblank=1 so
         * boot code that polls for vblank doesn't spin forever. */
        if (addr >= 0xC00004 && addr <= 0xC00007) {
            /* Bit 3 = vblank, bit 2 = hblank.  Return both set. */
            if (!(addr & 1))
                return 0x3E;  /* high byte of status: vblank + hblank + open bits */
            else
                return 0x00;  /* low byte */
        }
        return 0x00;
    }

    /* Work RAM: 0xE00000 - 0xFFFFFF (64 KB mirrored) */
    if (addr >= 0xE00000)
        return work_ram[addr & 0xFFFF];

    /* Unmapped */
    return 0xFF;
}

/* ------------------------------------------------------------------ */
/*  16-bit read                                                        */
/* ------------------------------------------------------------------ */

uint16_t bus_read16(uint32_t addr)
{
    /* Fast paths for the common regions to avoid two 8-bit lookups */

    if (addr < 0x400000) {
        if (addr + 1 < cart_rom_size)
            return ((uint16_t)cart_rom[addr] << 8) | cart_rom[addr + 1];
        if (addr < cart_rom_size)
            return ((uint16_t)cart_rom[addr] << 8) | 0xFF;
        return 0xFFFF;
    }

    if (addr >= 0xE00000) {
        uint16_t a = addr & 0xFFFF;
        return ((uint16_t)work_ram[a] << 8) | work_ram[(a + 1) & 0xFFFF];
    }

    /* VDP status register: return vblank set */
    if (addr >= 0xC00004 && addr <= 0xC00006)
        return 0x3E00;

    /* Everything else: compose from two 8-bit reads */
    return ((uint16_t)bus_read8(addr) << 8) | bus_read8(addr + 1);
}

/* ------------------------------------------------------------------ */
/*  32-bit read                                                        */
/* ------------------------------------------------------------------ */

uint32_t bus_read32(uint32_t addr)
{
    return ((uint32_t)bus_read16(addr) << 16) | bus_read16(addr + 2);
}

/* ------------------------------------------------------------------ */
/*  8-bit write                                                        */
/* ------------------------------------------------------------------ */

void bus_write8(uint32_t addr, uint8_t val)
{
    /* ROM area: writes ignored */
    if (addr < 0x400000)
        return;

    /* Z80 space: stub */
    if (addr >= 0xA00000 && addr <= 0xA0FFFF)
        return;

    /* I/O area: stub — accept and ignore */
    if (addr >= 0xA10000 && addr <= 0xA1001F)
        return;

    /* Z80 bus request / reset: stub */
    if (addr >= 0xA11100 && addr <= 0xA11201)
        return;

    /* VDP: stub — accept and ignore */
    if (addr >= 0xC00000 && addr <= 0xC0000F)
        return;

    /* Work RAM */
    if (addr >= 0xE00000) {
        work_ram[addr & 0xFFFF] = val;
        return;
    }
}

/* ------------------------------------------------------------------ */
/*  16-bit write                                                       */
/* ------------------------------------------------------------------ */

void bus_write16(uint32_t addr, uint16_t val)
{
    if (addr < 0x400000)
        return;

    if (addr >= 0xE00000) {
        uint16_t a = addr & 0xFFFF;
        work_ram[a] = val >> 8;
        work_ram[(a + 1) & 0xFFFF] = val & 0xFF;
        return;
    }

    /* VDP, I/O, Z80: stub — accept and ignore */
    if ((addr >= 0xC00000 && addr <= 0xC0000F) ||
        (addr >= 0xA10000 && addr <= 0xA1001F) ||
        (addr >= 0xA00000 && addr <= 0xA0FFFF) ||
        (addr >= 0xA11100 && addr <= 0xA11201))
        return;

    bus_write8(addr, val >> 8);
    bus_write8(addr + 1, val & 0xFF);
}

/* ------------------------------------------------------------------ */
/*  32-bit write                                                       */
/* ------------------------------------------------------------------ */

void bus_write32(uint32_t addr, uint32_t val)
{
    bus_write16(addr, val >> 16);
    bus_write16(addr + 2, val & 0xFFFF);
}

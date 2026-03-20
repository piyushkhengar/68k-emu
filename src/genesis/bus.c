/*
 * Genesis bus: address decoding for the 68000 side.
 *
 * Routes reads and writes to cartridge ROM, work RAM, VDP, I/O, or the
 * Z80 address space.  VDP accesses dispatch to vdp.c, I/O and system
 * registers dispatch to io.c.  Z80 RAM (8 KB) is backed by real storage
 * so the 68K can load driver code and poll handshake flags.
 */

#include "bus.h"
#include "cpu.h"
#include "vdp.h"
#include "io.h"
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
/*  Z80 RAM: 8 KB at 0xA00000, visible to the 68K when bus is granted */
/* ------------------------------------------------------------------ */

#define Z80_RAM_SIZE  0x2000
static uint8_t z80_ram[Z80_RAM_SIZE];

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
    memset(z80_ram, 0, Z80_RAM_SIZE);
    vdp_init();
    io_init();
    cpu_set_int_ack(vdp_int_ack);
    return 0;
}

void bus_reset(void)
{
    memset(work_ram, 0, WRAM_SIZE);
    memset(z80_ram, 0, Z80_RAM_SIZE);
    vdp_reset();
    io_reset();
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

    /* Z80 address space: 0xA00000 - 0xA0FFFF */
    if (addr >= 0xA00000 && addr <= 0xA0FFFF) {
        uint16_t z_addr = addr & 0xFFFF;
        if (z_addr < Z80_RAM_SIZE)
            return z80_ram[z_addr];
        if (z_addr >= 0x4000 && z_addr <= 0x4003)
            return 0x00;  /* YM2612 status: not busy */
        return 0xFF;
    }

    /* I/O and system registers: 0xA10000 - 0xA1FFFF */
    if (addr >= 0xA10000 && addr < 0xA20000)
        return io_read8(addr);

    /* VDP: 0xC00000 - 0xDFFFFF (mirrored every 32 bytes) */
    if (addr >= 0xC00000 && addr < 0xE00000) {
        uint16_t val;
        uint32_t port = addr & 0x1F;

        if (port < 0x04)
            val = vdp_data_read();
        else if (port < 0x08)
            val = vdp_control_read();
        else if (port < 0x10)
            val = vdp_hv_read();
        else
            return 0xFF;

        return (addr & 1) ? (val & 0xFF) : (val >> 8);
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

    /* VDP: 0xC00000 - 0xDFFFFF */
    if (addr >= 0xC00000 && addr < 0xE00000) {
        uint32_t port = addr & 0x1F;
        if (port < 0x04) return vdp_data_read();
        if (port < 0x08) return vdp_control_read();
        if (port < 0x10) return vdp_hv_read();
        return 0xFFFF;
    }

    if (addr >= 0xE00000) {
        uint16_t a = addr & 0xFFFF;
        return ((uint16_t)work_ram[a] << 8) | work_ram[(a + 1) & 0xFFFF];
    }

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

    /* Z80 address space: 0xA00000 - 0xA0FFFF */
    if (addr >= 0xA00000 && addr <= 0xA0FFFF) {
        uint16_t z_addr = addr & 0xFFFF;
        if (z_addr < Z80_RAM_SIZE)
            z80_ram[z_addr] = val;
        return;
    }

    /* I/O and system registers: 0xA10000 - 0xA1FFFF */
    if (addr >= 0xA10000 && addr < 0xA20000) {
        io_write8(addr, val);
        return;
    }

    /* VDP: 0xC00000 - 0xDFFFFF */
    if (addr >= 0xC00000 && addr < 0xE00000) {
        uint32_t port = addr & 0x1F;
        if (port < 0x04)
            vdp_data_write(((uint16_t)val << 8) | val);
        else if (port < 0x08)
            vdp_control_write(((uint16_t)val << 8) | val);
        return;
    }

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

    /* VDP: 0xC00000 - 0xDFFFFF */
    if (addr >= 0xC00000 && addr < 0xE00000) {
        uint32_t port = addr & 0x1F;
        if (port < 0x04)
            vdp_data_write(val);
        else if (port < 0x08)
            vdp_control_write(val);
        return;
    }

    /* Z80 address space: route as two byte writes */
    if (addr >= 0xA00000 && addr <= 0xA0FFFF) {
        bus_write8(addr, val >> 8);
        bus_write8(addr + 1, val & 0xFF);
        return;
    }

    /* I/O and system registers: route as two byte writes */
    if (addr >= 0xA10000 && addr < 0xA20000) {
        io_write8(addr, val >> 8);
        io_write8(addr + 1, val & 0xFF);
        return;
    }

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

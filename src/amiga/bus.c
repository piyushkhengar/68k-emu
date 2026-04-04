/*
 * Amiga 500 bus — address decoder and memory dispatch.
 *
 * Phase 1 scope: Chip RAM, Kickstart ROM, Gary OVL overlay, Slow RAM,
 * and stub responses for CIA and custom chip registers.  Custom chip and
 * CIA handlers will be replaced by real implementations in later phases.
 */

#include "bus.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ------------------------------------------------------------------ */
/*  Chip RAM (512 KB)                                                  */
/* ------------------------------------------------------------------ */

#define CHIP_RAM_SIZE  0x80000u
static uint8_t chip_ram[CHIP_RAM_SIZE];

/* ------------------------------------------------------------------ */
/*  Slow RAM (512 KB trapdoor expansion at 0xC00000)                   */
/* ------------------------------------------------------------------ */

#define SLOW_RAM_SIZE  0x80000u
#define SLOW_RAM_BASE  0xC00000u
static uint8_t slow_ram[SLOW_RAM_SIZE];

/* ------------------------------------------------------------------ */
/*  Kickstart ROM                                                       */
/* ------------------------------------------------------------------ */

#define ROM_WINDOW  0x80000u   /* 512 KB window */
#define ROM_BASE    0xF80000u

static uint8_t *kickstart_rom;
static uint32_t rom_size;

static uint8_t rom_read8(uint32_t offset)
{
    offset &= (ROM_WINDOW - 1);
    if (rom_size == 0)
        return 0xFF;
    /* Handles 256 KB ROMs (mirrored twice in the 512 KB window). */
    return kickstart_rom[offset % rom_size];
}

/* ------------------------------------------------------------------ */
/*  Gary OVL overlay flag                                              */
/* ------------------------------------------------------------------ */

static bool ovl_active;

void amiga_bus_set_ovl(bool active)
{
    ovl_active = active;
}

/* ------------------------------------------------------------------ */
/*  Init / Reset                                                        */
/* ------------------------------------------------------------------ */

int amiga_bus_init(const uint8_t *rom_data, size_t size)
{
    free(kickstart_rom);
    kickstart_rom = NULL;
    rom_size = 0;

    if (!rom_data || size == 0) {
        fprintf(stderr, "amiga bus_init: no ROM data\n");
        return -1;
    }

    kickstart_rom = (uint8_t *)malloc(size);
    if (!kickstart_rom) {
        fprintf(stderr, "amiga bus_init: out of memory\n");
        return -1;
    }
    memcpy(kickstart_rom, rom_data, size);
    rom_size = (uint32_t)size;

    memset(chip_ram, 0, sizeof(chip_ram));
    memset(slow_ram, 0, sizeof(slow_ram));
    ovl_active = true;
    return 0;
}

void amiga_bus_reset(void)
{
    memset(chip_ram, 0, sizeof(chip_ram));
    memset(slow_ram, 0, sizeof(slow_ram));
    ovl_active = true;
}

/* ------------------------------------------------------------------ */
/*  Read                                                                */
/* ------------------------------------------------------------------ */

uint8_t amiga_bus_read8(uint32_t addr)
{
    addr &= 0xFFFFFFu;  /* 24-bit address bus */

    /* Gary OVL: ROM mirrored at Chip RAM window on reset. */
    if (ovl_active && addr < 0x080000u)
        return rom_read8(addr);

    /* Chip RAM: 0x000000–0x07FFFF */
    if (addr < 0x080000u)
        return chip_ram[addr & (CHIP_RAM_SIZE - 1)];

    /* Open bus: 0x080000–0xBFCFFF */
    if (addr < 0xBFD000u)
        return 0xFF;

    /* CIA-B: 0xBFD000–0xBFDFFF (even bytes) — Phase 1 stub */
    if (addr < 0xBFE000u)
        return 0xFF;

    /* CIA-A: 0xBFE000–0xBFEFFF (odd bytes) — Phase 1 stub */
    if (addr < 0xBFF000u)
        return 0xFF;

    /* Open bus gap: 0xBFF000–0xBFFFFF */
    if (addr < 0xC00000u)
        return 0xFF;

    /* Slow RAM: 0xC00000–0xC7FFFF */
    if (addr < 0xC80000u)
        return slow_ram[addr - SLOW_RAM_BASE];

    /* Open bus gap: 0xC80000–0xDFEFFF */
    if (addr < 0xDFF000u)
        return 0xFF;

    /* Custom chip registers: 0xDFF000–0xDFFFFF — Phase 1 stub */
    if (addr < 0xE00000u)
        return 0x00;

    /* Open bus gap: 0xE00000–0xF7FFFF */
    if (addr < 0xF80000u)
        return 0xFF;

    /* Kickstart ROM: 0xF80000–0xFFFFFF */
    return rom_read8(addr - ROM_BASE);
}

uint16_t amiga_bus_read16(uint32_t addr)
{
    return ((uint16_t)amiga_bus_read8(addr) << 8) | amiga_bus_read8(addr + 1);
}

uint32_t amiga_bus_read32(uint32_t addr)
{
    return ((uint32_t)amiga_bus_read16(addr) << 16) | amiga_bus_read16(addr + 2);
}

/* ------------------------------------------------------------------ */
/*  Write                                                               */
/* ------------------------------------------------------------------ */

void amiga_bus_write8(uint32_t addr, uint8_t val)
{
    addr &= 0xFFFFFFu;

    /* Chip RAM: always writable regardless of OVL state. */
    if (addr < 0x080000u) {
        chip_ram[addr & (CHIP_RAM_SIZE - 1)] = val;
        return;
    }

    if (addr < 0xBFD000u) return;  /* open bus */

    /* CIA-B: 0xBFD000–0xBFDFFF — Phase 1 stub, ignore writes */
    if (addr < 0xBFE000u)
        return;

    /* CIA-A: 0xBFE000–0xBFEFFF */
    if (addr < 0xBFF000u) {
        /* reg = bits [11:8] of address (Amiga hardware convention). */
        uint8_t reg = (addr >> 8) & 0xF;
        if (reg == 0 && (val & 0x01))   /* PRA bit 0 = OVL */
            amiga_bus_set_ovl(false);
        return;
    }

    if (addr < 0xC00000u) return;

    /* Slow RAM: 0xC00000–0xC7FFFF */
    if (addr < 0xC80000u) {
        slow_ram[addr - SLOW_RAM_BASE] = val;
        return;
    }

    if (addr < 0xDFF000u) return;

    /* Custom chip registers: 0xDFF000–0xDFFFFF — Phase 1 stub, ignore */
    if (addr < 0xE00000u)
        return;

    /* ROM and everything else: writes silently ignored */
}

void amiga_bus_write16(uint32_t addr, uint16_t val)
{
    amiga_bus_write8(addr,     (val >> 8) & 0xFF);
    amiga_bus_write8(addr + 1,  val & 0xFF);
}

void amiga_bus_write32(uint32_t addr, uint32_t val)
{
    amiga_bus_write16(addr,     (val >> 16) & 0xFFFF);
    amiga_bus_write16(addr + 2,  val & 0xFFFF);
}

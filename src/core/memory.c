#include "memory.h"
#include "cpu_internal.h"
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/*  Flat-RAM backend (default — used by tests and standalone ROMs)     */
/* ------------------------------------------------------------------ */

static uint8_t *ram;

void mem_init(void)
{
    ram = (uint8_t *)calloc(MEM_SIZE, 1);
}

void mem_reset(void)
{
    if (ram)
        memset(ram, 0, MEM_SIZE);
}

void mem_load_rom(const uint8_t *data, size_t size)
{
    if (!ram || !data)
        return;
    size_t copy = size < MEM_SIZE ? size : MEM_SIZE;
    memcpy(ram, data, copy);
}

/* 68000 has 24-bit address bus */
#define ADDR_MASK24(addr) ((addr) & 0xFFFFFF)

static uint8_t flat_read8(uint32_t addr)
{
    return (addr < MEM_SIZE) ? ram[addr] : 0;
}

static uint16_t flat_read16(uint32_t addr)
{
    if (addr >= MEM_SIZE - 1)
        return 0;
    return (ram[addr] << 8) | ram[addr + 1];
}

static uint32_t flat_read32(uint32_t addr)
{
    if (addr >= MEM_SIZE - 3)
        return 0;
    return (ram[addr] << 24) | (ram[addr + 1] << 16) |
           (ram[addr + 2] << 8) | ram[addr + 3];
}

static void flat_write8(uint32_t addr, uint8_t val)
{
    if (addr < MEM_SIZE)
        ram[addr] = val;
}

static void flat_write16(uint32_t addr, uint16_t val)
{
    if (addr < MEM_SIZE - 1) {
        ram[addr] = val >> 8;
        ram[addr + 1] = val & 0xFF;
    }
}

static void flat_write32(uint32_t addr, uint32_t val)
{
    if (addr < MEM_SIZE - 3) {
        ram[addr]     = val >> 24;
        ram[addr + 1] = (val >> 16) & 0xFF;
        ram[addr + 2] = (val >> 8) & 0xFF;
        ram[addr + 3] = val & 0xFF;
    }
}

/* ------------------------------------------------------------------ */
/*  Bus dispatch (function pointers)                                   */
/* ------------------------------------------------------------------ */

static const mem_bus_t *active_bus;

void mem_set_bus(const mem_bus_t *bus)
{
    active_bus = bus;
}

/* ------------------------------------------------------------------ */
/*  Public API — address masking, alignment checks, then dispatch      */
/* ------------------------------------------------------------------ */

uint8_t mem_read8(uint32_t addr)
{
    addr = ADDR_MASK24(addr);
    if (active_bus)
        return active_bus->read8(addr);
    return flat_read8(addr);
}

uint16_t mem_read16(uint32_t addr)
{
    uint32_t orig = addr;
    addr = ADDR_MASK24(addr);
    if (addr & 1) {
        cpu_take_addr_err_data(orig, 1);
        return 0;
    }
    if (active_bus)
        return active_bus->read16(addr);
    return flat_read16(addr);
}

uint32_t mem_read32(uint32_t addr)
{
    uint32_t orig = addr;
    addr = ADDR_MASK24(addr);
    if (addr & 1) {
        cpu_take_addr_err_data(orig, 1);
        return 0;
    }
    if (active_bus)
        return active_bus->read32(addr);
    return flat_read32(addr);
}

void mem_write8(uint32_t addr, uint8_t val)
{
    addr = ADDR_MASK24(addr);
    if (active_bus) {
        active_bus->write8(addr, val);
        return;
    }
    flat_write8(addr, val);
}

void mem_write16(uint32_t addr, uint16_t val)
{
    uint32_t orig = addr;
    addr = ADDR_MASK24(addr);
    if (addr & 1) {
        cpu_take_addr_err_data(orig, 0);
        return;
    }
    if (active_bus) {
        active_bus->write16(addr, val);
        return;
    }
    flat_write16(addr, val);
}

void mem_write32(uint32_t addr, uint32_t val)
{
    uint32_t orig = addr;
    addr = ADDR_MASK24(addr);
    if (addr & 1) {
        cpu_take_addr_err_data(orig, 0);
        return;
    }
    if (active_bus) {
        active_bus->write32(addr, val);
        return;
    }
    flat_write32(addr, val);
}

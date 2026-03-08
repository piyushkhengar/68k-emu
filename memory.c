#include "memory.h"
#include "cpu_internal.h"
#include <stdlib.h>
#include <string.h>

#define MEM_SIZE (16 * 1024 * 1024)  /* 16MB - enough for test ROMs */

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

uint8_t mem_read8(uint32_t addr)
{
    if (addr >= MEM_SIZE)
        return 0;
    return ram[addr];
}

uint16_t mem_read16(uint32_t addr)
{
    if (addr & 1) {
        cpu_take_exception(ADDR_ERR_VECTOR);
        return 0;  /* unreachable */
    }
    if (addr >= MEM_SIZE - 1)
        return 0;
    return (ram[addr] << 8) | ram[addr + 1];
}

uint32_t mem_read32(uint32_t addr)
{
    if (addr & 1) {
        cpu_take_exception(ADDR_ERR_VECTOR);
        return 0;  /* unreachable */
    }
    if (addr >= MEM_SIZE - 3)
        return 0;
    return (ram[addr] << 24) | (ram[addr + 1] << 16) | (ram[addr + 2] << 8) | ram[addr + 3];
}

void mem_write8(uint32_t addr, uint8_t val)
{
    if (addr < MEM_SIZE)
        ram[addr] = val;
}

void mem_write16(uint32_t addr, uint16_t val)
{
    if (addr & 1) {
        cpu_take_exception(ADDR_ERR_VECTOR);
        return;
    }
    if (addr < MEM_SIZE - 1) {
        ram[addr] = val >> 8;
        ram[addr + 1] = val & 0xFF;
    }
}

void mem_write32(uint32_t addr, uint32_t val)
{
    if (addr & 1) {
        cpu_take_exception(ADDR_ERR_VECTOR);
        return;
    }
    if (addr < MEM_SIZE - 3) {
        ram[addr] = val >> 24;
        ram[addr + 1] = (val >> 16) & 0xFF;
        ram[addr + 2] = (val >> 8) & 0xFF;
        ram[addr + 3] = val & 0xFF;
    }
}

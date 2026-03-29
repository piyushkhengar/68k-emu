/* bare-metal/kernel/bus_bare.c
 * Bare-metal replacement for src/genesis/bus.c.
 * Identical routing logic; differences:
 *   - No malloc/free: holds a const pointer to the embedded ROM instead of
 *     copying it into a heap allocation.
 *   - fprintf/stderr → kprintf.
 */

#include "bus.h"
#include "cpu.h"
#include "vdp.h"
#include "io.h"
#include "z80.h"
#include "psg.h"
#include "ym2612.h"
#include "bare.h"

/* ---- Cartridge ROM (pointer into .rom_data, no copy) -------------------- */
static const uint8_t *cart_rom;
static uint32_t       cart_rom_size;

/* ---- 68K work RAM: 64 KB at 0xE00000, mirrored through 0xFFFFFF --------- */
#define WRAM_SIZE 0x10000
static uint8_t work_ram[WRAM_SIZE];

/* ---- Init / Reset -------------------------------------------------------- */

int bus_init(const uint8_t *rom_data, size_t rom_size)
{
    cart_rom      = NULL;
    cart_rom_size = 0;

    if (!rom_data || rom_size == 0) {
        kprintf("bus_init: no ROM data\n");
        return -1;
    }

    /* Point directly at the caller's ROM (embedded array or GRUB module).
     * No malloc needed — the data lives in .rom_data section. */
    cart_rom      = rom_data;
    cart_rom_size = (uint32_t)rom_size;

    memset(work_ram, 0, WRAM_SIZE);
    z80_init();
    psg_init();
    ym2612_init();
    vdp_init();
    io_init();
    cpu_set_int_ack(vdp_int_ack);
    return 0;
}

void bus_reset(void)
{
    memset(work_ram, 0, WRAM_SIZE);
    z80_reset();
    psg_reset();
    ym2612_reset();
    vdp_reset();
    io_reset();
}

/* ---- 8-bit read ---------------------------------------------------------- */

uint8_t bus_read8(uint32_t addr)
{
    if (addr < 0x400000) {
        if (addr < cart_rom_size)
            return cart_rom[addr];
        return 0xFF;
    }

    if (addr >= 0xA00000 && addr <= 0xA0FFFF) {
        uint16_t z_addr = addr & 0xFFFF;
        if (z_addr < 0x2000)
            return z80_ram_read(z_addr);
        if (z_addr >= 0x4000 && z_addr <= 0x4003)
            return ym2612_read();
        return 0xFF;
    }

    if (addr >= 0xA10000 && addr < 0xA20000)
        return io_read8(addr);

    if (addr >= 0xC00000 && addr < 0xE00000) {
        uint16_t val;
        uint32_t port = addr & 0x1F;
        if (port < 0x04)       val = vdp_data_read();
        else if (port < 0x08)  val = vdp_control_read();
        else if (port < 0x10)  val = vdp_hv_read();
        else                   return 0xFF;
        return (addr & 1) ? (val & 0xFF) : (val >> 8);
    }

    if (addr >= 0xE00000)
        return work_ram[addr & 0xFFFF];

    return 0xFF;
}

/* ---- 16-bit read --------------------------------------------------------- */

uint16_t bus_read16(uint32_t addr)
{
    if (addr < 0x400000) {
        if (addr + 1 < cart_rom_size)
            return ((uint16_t)cart_rom[addr] << 8) | cart_rom[addr + 1];
        if (addr < cart_rom_size)
            return ((uint16_t)cart_rom[addr] << 8) | 0xFF;
        return 0xFFFF;
    }

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

    return ((uint16_t)bus_read8(addr) << 8) | bus_read8(addr + 1);
}

/* ---- 32-bit read --------------------------------------------------------- */

uint32_t bus_read32(uint32_t addr)
{
    return ((uint32_t)bus_read16(addr) << 16) | bus_read16(addr + 2);
}

/* ---- 8-bit write --------------------------------------------------------- */

void bus_write8(uint32_t addr, uint8_t val)
{
    if (addr < 0x400000)
        return;

    if (addr >= 0xA00000 && addr <= 0xA0FFFF) {
        uint16_t z_addr = addr & 0xFFFF;
        if (z_addr < 0x2000)
            z80_ram_write(z_addr, val);
        else if (z_addr >= 0x4000 && z_addr <= 0x4003)
            ym2612_write(z_addr & 3, val);
        return;
    }

    if (addr >= 0xA10000 && addr < 0xA20000) {
        io_write8(addr, val);
        return;
    }

    if (addr >= 0xC00000 && addr < 0xE00000) {
        uint32_t port = addr & 0x1F;
        if (port < 0x04)
            vdp_data_write(((uint16_t)val << 8) | val);
        else if (port < 0x08)
            vdp_control_write(((uint16_t)val << 8) | val);
        else if (port == 0x11)
            psg_write(val);
        return;
    }

    if (addr >= 0xE00000) {
        work_ram[addr & 0xFFFF] = val;
        return;
    }
}

/* ---- 16-bit write -------------------------------------------------------- */

void bus_write16(uint32_t addr, uint16_t val)
{
    if (addr < 0x400000)
        return;

    if (addr >= 0xE00000) {
        uint16_t a = addr & 0xFFFF;
        work_ram[a]            = val >> 8;
        work_ram[(a+1) & 0xFFFF] = val & 0xFF;
        return;
    }

    if (addr >= 0xC00000 && addr < 0xE00000) {
        uint32_t port = addr & 0x1F;
        if (port < 0x04)
            vdp_data_write(val);
        else if (port < 0x08)
            vdp_control_write(val);
        else if (port == 0x10 || port == 0x11)
            psg_write(val & 0xFF);
        return;
    }

    if (addr >= 0xA00000 && addr <= 0xA0FFFF) {
        bus_write8(addr, val >> 8);
        bus_write8(addr + 1, val & 0xFF);
        return;
    }

    if (addr >= 0xA10000 && addr < 0xA20000) {
        io_write8(addr, val >> 8);
        io_write8(addr + 1, val & 0xFF);
        return;
    }

    bus_write8(addr, val >> 8);
    bus_write8(addr + 1, val & 0xFF);
}

/* ---- 32-bit write -------------------------------------------------------- */

void bus_write32(uint32_t addr, uint32_t val)
{
    bus_write16(addr,     val >> 16);
    bus_write16(addr + 2, val & 0xFFFF);
}

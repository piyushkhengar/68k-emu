/*
 * Amiga 500 bus — address decoder and memory dispatch.
 *
 * Chapter 2: CIA-A/B fully wired; custom chip registers dispatched to Paula.
 * Agnus and Denise dispatch will be added in Chapter 3.
 */

#include "bus.h"
#include "cia.h"
#include "paula.h"
#include "agnus.h"
#include "denise.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ------------------------------------------------------------------ */
/*  Chip singletons (shared with amiga.c via bus.h externs)            */
/* ------------------------------------------------------------------ */

paula_t  amiga_paula;
cia_t    amiga_cia_a;
cia_t    amiga_cia_b;
agnus_t  amiga_agnus;
denise_t amiga_denise;

/* ------------------------------------------------------------------ */
/*  Chip RAM (512 KB)                                                  */
/* ------------------------------------------------------------------ */

#define CHIP_RAM_SIZE  0x80000u
static uint8_t chip_ram[CHIP_RAM_SIZE];

/* ------------------------------------------------------------------ */
/*  Slow RAM (disabled: base A500 has only 512 KB chip RAM)            */
/*                                                                      */
/*  Kickstart's ExecBase validity check (FC303A: ANDI.L #$00FF0001)    */
/*  requires ExecBase in the $000000-$00FFFF00 range.  If slow RAM at  */
/*  $C00000 is present, the memory sizer may place ExecBase there,     */
/*  which fails the check and sends boot into an infinite restart.     */
/*  Disable slow RAM until the memory sizer is better understood.      */
/* ------------------------------------------------------------------ */

#if 0  /* slow RAM disabled for now */
#define SLOW_RAM_SIZE  0x80000u
#define SLOW_RAM_BASE  0xC00000u
static uint8_t slow_ram[SLOW_RAM_SIZE];
#endif

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
    /* slow_ram disabled — see comment at declaration */
    ovl_active = true;

    paula_init(&amiga_paula);
    cia_init(&amiga_cia_a);
    cia_init(&amiga_cia_b);
    agnus_init(&amiga_agnus);
    denise_init(&amiga_denise);
    return 0;
}

void amiga_bus_reset(void)
{
    memset(chip_ram, 0, sizeof(chip_ram));
    /* slow_ram disabled — see comment at declaration */
    ovl_active = true;

    paula_reset(&amiga_paula);
    cia_reset(&amiga_cia_a);
    cia_reset(&amiga_cia_b);
    agnus_reset(&amiga_agnus);
    denise_reset(&amiga_denise);
}

/* ------------------------------------------------------------------ */
/*  Chip RAM accessor (used by amiga.c to pass to denise_render_line)  */
/* ------------------------------------------------------------------ */

const uint8_t *amiga_bus_chip_ram(void)      { return chip_ram; }
uint32_t       amiga_bus_chip_ram_size(void) { return CHIP_RAM_SIZE; }

/* ------------------------------------------------------------------ */
/*  Custom chip register dispatch (offset from 0xDFF000 base)          */
/*                                                                      */
/*  Ownership:                                                          */
/*    Agnus  reads : DMACONR (0x002), VPOSR (0x004), VHPOSR (0x006)   */
/*    Agnus  writes: DMACON  (0x096)                                   */
/*    Denise reads : COLOR00–31 (0x180–0x1BE)                          */
/*    Denise writes: BPLxPT (0x0E0–0x0F6), BPLCON0–2 (0x100–0x104),  */
/*                   COLOR00–31 (0x180–0x1BE)                          */
/*    Paula         everything else                                     */
/* ------------------------------------------------------------------ */

static uint16_t custom_read_reg(uint16_t off)
{
    /* Agnus-owned reads */
    if (off == 0x002 || off == 0x004 || off == 0x006)
        return agnus_read_reg(&amiga_agnus, off);

    /* Denise-owned reads (colour registers are readable) */
    if (off >= 0x180u && off <= 0x1BEu)
        return denise_read_reg(&amiga_denise, off);

    /* Paula handles everything else */
    return paula_read_reg(&amiga_paula, off);
}

static void custom_write_reg(uint16_t off, uint16_t val)
{
    /* ---- Agnus: Blitter registers (0x040–0x074) ------------------- */
    if ((off >= 0x040u && off <= 0x066u) ||
        (off >= 0x070u && off <= 0x074u)) {
        if (off == 0x058u) {
            /* BLTSIZE write triggers synchronous blit then fires IRQ */
            agnus_blitter_execute(&amiga_agnus, chip_ram, CHIP_RAM_SIZE, val);
            paula_assert_intreq(&amiga_paula, INTREQ_BLIT);
        } else {
            agnus_write_reg(&amiga_agnus, off, val);
        }
        return;
    }

    /* ---- Agnus: Copper registers (0x080–0x08A) -------------------- */
    if (off >= 0x080u && off <= 0x08Au) {
        agnus_write_reg(&amiga_agnus, off, val);
        return;
    }

    /* ---- Agnus: DMACON (0x096) ------------------------------------ */
    if (off == 0x096u) {
        agnus_write_reg(&amiga_agnus, off, val);
        return;
    }

    /* ---- Denise: BPLxPT pointers, BPLCON0–2, sprites, palette ---- */
    if ((off >= 0x0E0u && off <= 0x0F6u) ||
         off == 0x100u || off == 0x102u || off == 0x104u ||
        (off >= 0x140u && off <= 0x17Eu) ||
        (off >= 0x180u && off <= 0x1BEu)) {
        denise_write_reg(&amiga_denise, off, val);
        return;
    }

    /* ---- Paula: INTENA, INTREQ, ADKCON, audio, … ------------------ */
    paula_write_reg(&amiga_paula, off, val);
}

/* Public wrapper used by the Copper write callback in amiga.c */
void amiga_bus_write_custom(uint16_t offset, uint16_t val)
{
    custom_write_reg(offset, val);
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

    /* CIA-B: 0xBFD000–0xBFDFFF (even bytes) */
    if (addr < 0xBFE000u) {
        uint8_t reg = (uint8_t)((addr >> 8) & 0xFu);
        return cia_read(&amiga_cia_b, reg);
    }

    /* CIA-A: 0xBFE000–0xBFEFFF (odd bytes) */
    if (addr < 0xBFF000u) {
        uint8_t reg = (uint8_t)((addr >> 8) & 0xFu);
        return cia_read(&amiga_cia_a, reg);
    }

    /* Open bus gap: 0xBFF000–0xBFFFFF */
    if (addr < 0xC00000u)
        return 0xFF;

    /* Open bus: 0xC00000–0xDFEFFF (slow RAM disabled) */
    if (addr < 0xDFF000u)
        return 0xFF;

    /* Custom chip registers: 0xDFF000–0xDFFFFF.
     * Registers are 16-bit; 8-bit reads return the appropriate byte. */
    if (addr < 0xE00000u) {
        uint16_t off = (uint16_t)(addr & 0xFFEu);  /* word-align */
        uint16_t val = custom_read_reg(off);
        return (addr & 1u) ? (uint8_t)(val & 0xFFu) : (uint8_t)(val >> 8);
    }

    /* Open bus gap: 0xE00000–0xF7FFFF */
    if (addr < 0xF80000u)
        return 0xFF;

    /* Kickstart ROM: 0xF80000–0xFFFFFF */
    return rom_read8(addr - ROM_BASE);
}

uint16_t amiga_bus_read16(uint32_t addr)
{
    addr &= 0xFFFFFFu;
    /* Custom chip registers return a 16-bit value; read atomically. */
    if (addr >= 0xDFF000u && addr < 0xE00000u)
        return custom_read_reg((uint16_t)(addr & 0xFFEu));
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

    /* CIA-B: 0xBFD000–0xBFDFFF */
    if (addr < 0xBFE000u) {
        uint8_t reg = (uint8_t)((addr >> 8) & 0xFu);
        cia_write(&amiga_cia_b, reg, val);
        return;
    }

    /* CIA-A: 0xBFE000–0xBFEFFF */
    if (addr < 0xBFF000u) {
        uint8_t reg = (uint8_t)((addr >> 8) & 0xFu);
        cia_write(&amiga_cia_a, reg, val);
        /* Recalculate OVL after any PRA or DDRA write.
         * OVL is directly driven by CIA-A PRA bit 0:
         *   DDRA bit 0 = 0 (input)  → external pull-up → HIGH → OVL active
         *   DDRA bit 0 = 1 (output) → pin = PRA bit 0
         *     PRA bit 0 = 1 → HIGH → OVL active
         *     PRA bit 0 = 0 → LOW  → OVL deactivated */
        if (reg == CIA_PRA || reg == CIA_DDRA) {
            bool pin0 = (amiga_cia_a.ddra & 0x01u)
                      ? (amiga_cia_a.pra & 0x01u)   /* output: driven by PRA */
                      : 1u;                           /* input: pull-up = HIGH */
            amiga_bus_set_ovl(pin0);
        }
        return;
    }

    if (addr < 0xC00000u) return;

    /* Open bus: slow RAM disabled */
    if (addr < 0xDFF000u) return;

    /* Custom chip registers: 0xDFF000–0xDFFFFF.
     * Custom chip registers are 16-bit write-only; 8-bit writes are
     * not supported by the hardware and silently ignored here.
     * Use amiga_bus_write16() for correct dispatch.
     * Agnus and Denise dispatch will be added in Chapter 3. */
    if (addr < 0xE00000u)
        return;

    /* ROM and everything else: writes silently ignored */
}

void amiga_bus_write16(uint32_t addr, uint16_t val)
{
    addr &= 0xFFFFFFu;
    /* Custom chip registers are 16-bit; dispatch atomically to preserve the
     * SET/CLR bit 15 semantics. */
    if (addr >= 0xDFF000u && addr < 0xE00000u) {
        custom_write_reg((uint16_t)(addr & 0xFFEu), val);
        return;
    }
    amiga_bus_write8(addr,     (uint8_t)((val >> 8) & 0xFFu));
    amiga_bus_write8(addr + 1, (uint8_t)(val & 0xFFu));
}

void amiga_bus_write32(uint32_t addr, uint32_t val)
{
    amiga_bus_write16(addr,     (val >> 16) & 0xFFFF);
    amiga_bus_write16(addr + 2,  val & 0xFFFF);
}

/*
 * Unit tests for the Amiga 500 bus (src/amiga/bus.c).
 *
 * Tests are written against the bus interface directly — no CPU, no memory
 * layer, no SDL2. Each test function is self-contained and resets bus state
 * with a known fake ROM before asserting.
 *
 * Fake ROM (16 bytes, power-of-2 so % wraps cleanly):
 *   Byte 0–3:  0xDE 0xAD 0xBE 0xEF   (SSP when OVL active)
 *   Byte 4–7:  0x00 0xFC 0x00 0x02   (PC  when OVL active)
 *   Byte 8–15: 0xAA 0xBB 0xCC 0xDD 0x11 0x22 0x33 0x44
 */

#include "bus.h"
#include "bus_tests.h"
#include <stdio.h>

/* ------------------------------------------------------------------ */
/*  Test framework (same style as timing_tests.c)                      */
/* ------------------------------------------------------------------ */

static int failures;
static int total;

#define BASSERT(expr, fmt, ...) do {                                    \
    total++;                                                            \
    if (!(expr)) {                                                      \
        failures++;                                                     \
        printf("    FAIL [%s:%d]: " fmt "\n", __func__, __LINE__,      \
               ##__VA_ARGS__);                                          \
    }                                                                   \
} while (0)

/* ------------------------------------------------------------------ */
/*  Shared fake ROM                                                     */
/* ------------------------------------------------------------------ */

static const uint8_t fake_rom[] = {
    0xDE, 0xAD, 0xBE, 0xEF,   /* bytes 0-3:  SSP high-bytes if read as OVL */
    0x00, 0xFC, 0x00, 0x02,   /* bytes 4-7:  PC if read as OVL */
    0xAA, 0xBB, 0xCC, 0xDD,   /* bytes 8-11 */
    0x11, 0x22, 0x33, 0x44,   /* bytes 12-15 */
};
#define FAKE_ROM_SIZE  ((int)(sizeof(fake_rom)))

static void reset_bus(void)
{
    amiga_bus_init(fake_rom, FAKE_ROM_SIZE);
}

/* ------------------------------------------------------------------ */
/*  Test: OVL is active right after amiga_bus_init                     */
/* ------------------------------------------------------------------ */

static void test_ovl_active_after_init(void)
{
    reset_bus();
    /* With OVL on, reads at 0x000000 must return ROM byte 0 (0xDE). */
    BASSERT(amiga_bus_read8(0x000000) == 0xDE,
            "OVL on: 0x000000 expected 0xDE, got 0x%02X",
            amiga_bus_read8(0x000000));
    BASSERT(amiga_bus_read8(0x000001) == 0xAD,
            "OVL on: 0x000001 expected 0xAD, got 0x%02X",
            amiga_bus_read8(0x000001));
}

/* ------------------------------------------------------------------ */
/*  Test: OVL maps full chip RAM window to ROM                         */
/* ------------------------------------------------------------------ */

static void test_ovl_maps_pc_vector(void)
{
    reset_bus();
    /* PC vector lives at addresses 4-7 in the reset vector table. */
    BASSERT(amiga_bus_read8(0x000004) == 0x00,
            "OVL PC[0]: expected 0x00, got 0x%02X", amiga_bus_read8(0x000004));
    BASSERT(amiga_bus_read8(0x000005) == 0xFC,
            "OVL PC[1]: expected 0xFC, got 0x%02X", amiga_bus_read8(0x000005));
}

/* ------------------------------------------------------------------ */
/*  Test: Reads at ROM base address return ROM data                    */
/* ------------------------------------------------------------------ */

static void test_rom_read_at_base(void)
{
    reset_bus();
    BASSERT(amiga_bus_read8(0xF80000) == 0xDE,
            "ROM[0]: expected 0xDE, got 0x%02X", amiga_bus_read8(0xF80000));
    BASSERT(amiga_bus_read8(0xF80001) == 0xAD,
            "ROM[1]: expected 0xAD, got 0x%02X", amiga_bus_read8(0xF80001));
    BASSERT(amiga_bus_read8(0xF80008) == 0xAA,
            "ROM[8]: expected 0xAA, got 0x%02X", amiga_bus_read8(0xF80008));
}

/* ------------------------------------------------------------------ */
/*  Test: ROM 16-bit read is big-endian                                */
/* ------------------------------------------------------------------ */

static void test_rom_read16_big_endian(void)
{
    reset_bus();
    /* ROM bytes 0-1 are 0xDE 0xAD → word must be 0xDEAD. */
    uint16_t w = amiga_bus_read16(0xF80000);
    BASSERT(w == 0xDEAD, "ROM word: expected 0xDEAD, got 0x%04X", w);
}

/* ------------------------------------------------------------------ */
/*  Test: Writes to ROM are silently ignored                           */
/* ------------------------------------------------------------------ */

static void test_rom_write_ignored(void)
{
    reset_bus();
    amiga_bus_write8(0xF80000, 0x99);
    BASSERT(amiga_bus_read8(0xF80000) == 0xDE,
            "ROM write-protect: expected 0xDE after write, got 0x%02X",
            amiga_bus_read8(0xF80000));
}

/* ------------------------------------------------------------------ */
/*  Test: amiga_bus_set_ovl(false) switches chip RAM window back       */
/* ------------------------------------------------------------------ */

static void test_ovl_clear_exposes_chip_ram(void)
{
    reset_bus();
    /* Chip RAM is zeroed by amiga_bus_init, so reads return 0. */
    amiga_bus_set_ovl(false);
    BASSERT(amiga_bus_read8(0x000000) == 0x00,
            "OVL off: 0x000000 expected 0x00 (chip RAM), got 0x%02X",
            amiga_bus_read8(0x000000));
}

/* ------------------------------------------------------------------ */
/*  Test: amiga_bus_reset re-activates OVL                             */
/* ------------------------------------------------------------------ */

static void test_reset_reactivates_ovl(void)
{
    reset_bus();
    amiga_bus_set_ovl(false);
    amiga_bus_reset();
    BASSERT(amiga_bus_read8(0x000000) == 0xDE,
            "After reset, OVL on: expected 0xDE, got 0x%02X",
            amiga_bus_read8(0x000000));
}

/* ------------------------------------------------------------------ */
/*  Test: Chip RAM read/write                                          */
/* ------------------------------------------------------------------ */

static void test_chip_ram_rw(void)
{
    reset_bus();
    amiga_bus_set_ovl(false);
    amiga_bus_write8(0x001000, 0x42);
    BASSERT(amiga_bus_read8(0x001000) == 0x42,
            "chip RAM byte: expected 0x42, got 0x%02X",
            amiga_bus_read8(0x001000));
    amiga_bus_write16(0x002000, 0xABCD);
    BASSERT(amiga_bus_read8(0x002000) == 0xAB,
            "chip RAM word high: expected 0xAB, got 0x%02X",
            amiga_bus_read8(0x002000));
    BASSERT(amiga_bus_read8(0x002001) == 0xCD,
            "chip RAM word low: expected 0xCD, got 0x%02X",
            amiga_bus_read8(0x002001));
    BASSERT(amiga_bus_read16(0x002000) == 0xABCD,
            "chip RAM read16: expected 0xABCD, got 0x%04X",
            amiga_bus_read16(0x002000));
}

/* ------------------------------------------------------------------ */
/*  Test: Chip RAM writes work even while OVL is active               */
/* ------------------------------------------------------------------ */

static void test_chip_ram_write_through_ovl(void)
{
    reset_bus();
    /* OVL is active — writes still go to chip RAM, not ROM. */
    amiga_bus_write8(0x000100, 0x77);
    /* Turn off OVL to verify the write landed in chip RAM. */
    amiga_bus_set_ovl(false);
    BASSERT(amiga_bus_read8(0x000100) == 0x77,
            "chip RAM write with OVL on: expected 0x77, got 0x%02X",
            amiga_bus_read8(0x000100));
}

/* ------------------------------------------------------------------ */
/*  Test: Slow RAM read/write                                          */
/* ------------------------------------------------------------------ */

static void test_slow_ram_rw(void)
{
    /* Slow RAM is disabled (bus.c #if 0) — see comment there about
     * ExecBase placement breaking Kickstart's validity check. The
     * 0xC00000–0xDFEFFF window currently reads as open bus (0xFF). */
    reset_bus();
    amiga_bus_write8(0xC00000, 0x5A);
    BASSERT(amiga_bus_read8(0xC00000) == 0xFF,
            "slow RAM disabled: expected 0xFF, got 0x%02X",
            amiga_bus_read8(0xC00000));
    amiga_bus_write8(0xC07FFF, 0xA5);
    BASSERT(amiga_bus_read8(0xC07FFF) == 0xFF,
            "slow RAM disabled: expected 0xFF, got 0x%02X",
            amiga_bus_read8(0xC07FFF));
}

/* ------------------------------------------------------------------ */
/*  Test: Open bus returns 0xFF                                        */
/* ------------------------------------------------------------------ */

static void test_open_bus_returns_ff(void)
{
    reset_bus();
    BASSERT(amiga_bus_read8(0x080000) == 0xFF,
            "open bus 0x080000: expected 0xFF, got 0x%02X",
            amiga_bus_read8(0x080000));
    BASSERT(amiga_bus_read8(0x0FFFFF) == 0xFF,
            "open bus 0x0FFFFF: expected 0xFF, got 0x%02X",
            amiga_bus_read8(0x0FFFFF));
}

/* ------------------------------------------------------------------ */
/*  Test: CIA-A stub returns 0xFF                                      */
/* ------------------------------------------------------------------ */

static void test_cia_a_stub_read(void)
{
    reset_bus();
    BASSERT(amiga_bus_read8(0xBFE001) == 0xFF,
            "CIA-A PRA: expected 0xFF, got 0x%02X",
            amiga_bus_read8(0xBFE001));
    BASSERT(amiga_bus_read8(0xBFE101) == 0xFF,
            "CIA-A PRB: expected 0xFF, got 0x%02X",
            amiga_bus_read8(0xBFE101));
}

/* ------------------------------------------------------------------ */
/*  Test: CIA-B stub returns 0xFF                                      */
/* ------------------------------------------------------------------ */

static void test_cia_b_stub_read(void)
{
    reset_bus();
    BASSERT(amiga_bus_read8(0xBFD000) == 0xFF,
            "CIA-B: expected 0xFF, got 0x%02X",
            amiga_bus_read8(0xBFD000));
}

/* ------------------------------------------------------------------ */
/*  Test: Custom chip register stub returns 0                          */
/* ------------------------------------------------------------------ */

static void test_custom_reg_stub_read(void)
{
    reset_bus();
    /*
     * 0xDFF002 = DMACONR high byte.
     * Bit 14 = BBUSY (1 = busy, 0 = idle). Synchronous blitter is
     * always idle, so the high byte = 0x00 at reset.
     */
    BASSERT(amiga_bus_read8(0xDFF002) == 0x00,
            "DMACONR high byte: expected 0x00 (BBUSY=0, idle), got 0x%02X",
            amiga_bus_read8(0xDFF002));
    BASSERT(amiga_bus_read8(0xDFF000) == 0x00,
            "custom 0xDFF000 stub: expected 0x00, got 0x%02X",
            amiga_bus_read8(0xDFF000));
}

/* ------------------------------------------------------------------ */
/*  Test: CIA-A DDRA+PRA matching Kickstart deactivates OVL           */
/*  Real hardware: OVL tracks effective pin level of CIA-A PRA bit 0.  */
/*  DDRA bit 0=output + PRA bit 0=0 → pin LOW → OVL deactivated.      */
/* ------------------------------------------------------------------ */

static void test_cia_a_pra_clears_ovl(void)
{
    reset_bus();
    /* With OVL on, address 0 reads ROM. */
    BASSERT(amiga_bus_read8(0x000000) == 0xDE, "pre-PRA: OVL active");

    /* Kickstart sets DDRA bits 0,1 as output, then writes PRA bit 0 = 0.
     * Since PRA defaults to 0 after reset, the DDRA write alone is enough
     * to drive the pin LOW and deactivate OVL. */
    amiga_bus_write8(0xBFE201, 0x03);  /* DDRA: bits 0,1 as output */
    amiga_bus_write8(0xBFE001, 0x02);  /* PRA: bit 1=1 (LED), bit 0=0 */

    /* Now 0x000000 must expose chip RAM (zeroed), not ROM. */
    BASSERT(amiga_bus_read8(0x000000) == 0x00,
            "post-PRA: OVL cleared, expected 0x00, got 0x%02X",
            amiga_bus_read8(0x000000));
}

/* ------------------------------------------------------------------ */
/*  Test: PRA write without DDRA output does NOT deactivate OVL       */
/*  When DDRA bit 0 = 0 (input), the external pull-up keeps OVL HIGH. */
/* ------------------------------------------------------------------ */

static void test_cia_a_pra_bit0_clear_keeps_ovl(void)
{
    reset_bus();
    /* DDRA defaults to 0 (all inputs) — pull-up keeps OVL active.
     * Writing PRA with bit 0 = 0 has no effect on the pin. */
    amiga_bus_write8(0xBFE001, 0xFE);
    BASSERT(amiga_bus_read8(0x000000) == 0xDE,
            "PRA bit0=0 (input pin) must not clear OVL, expected 0xDE, got 0x%02X",
            amiga_bus_read8(0x000000));
}

/* ------------------------------------------------------------------ */
/*  Test: 32-bit read across chip RAM                                  */
/* ------------------------------------------------------------------ */

static void test_chip_ram_read32(void)
{
    reset_bus();
    amiga_bus_set_ovl(false);
    amiga_bus_write8(0x003000, 0x12);
    amiga_bus_write8(0x003001, 0x34);
    amiga_bus_write8(0x003002, 0x56);
    amiga_bus_write8(0x003003, 0x78);
    BASSERT(amiga_bus_read32(0x003000) == 0x12345678,
            "chip RAM read32: expected 0x12345678, got 0x%08X",
            amiga_bus_read32(0x003000));
}

/* ------------------------------------------------------------------ */
/*  Entry point                                                        */
/* ------------------------------------------------------------------ */

int run_amiga_bus_tests(void)
{
    failures = 0;
    total    = 0;

    printf("Amiga bus tests:\n");

    test_ovl_active_after_init();
    test_ovl_maps_pc_vector();
    test_rom_read_at_base();
    test_rom_read16_big_endian();
    test_rom_write_ignored();
    test_ovl_clear_exposes_chip_ram();
    test_reset_reactivates_ovl();
    test_chip_ram_rw();
    test_chip_ram_write_through_ovl();
    test_slow_ram_rw();
    test_open_bus_returns_ff();
    test_cia_a_stub_read();
    test_cia_b_stub_read();
    test_custom_reg_stub_read();
    test_cia_a_pra_clears_ovl();
    test_cia_a_pra_bit0_clear_keeps_ovl();
    test_chip_ram_read32();

    if (failures == 0)
        printf("  Amiga bus: all %d assertions passed.\n", total);
    else
        printf("  Amiga bus: %d/%d assertions FAILED.\n", failures, total);

    return failures;
}

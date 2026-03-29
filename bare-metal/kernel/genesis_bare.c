/* bare-metal/kernel/genesis_bare.c
 * Bare-metal Genesis main loop — replacement for src/genesis/genesis.c.
 * No SDL, no fopen, no printf: uses kprintf + Genesis bus subsystem.
 */

#include "bare.h"
#include "cpu.h"
#include "memory.h"
#include "bus.h"
#include "vdp.h"
#include "z80.h"
#include "io.h"
#include "ym2612.h"
#include "genesis_bare.h"

/* ---- Genesis bus (routes CPU reads/writes to bus_bare.c) ---------------- */
static const mem_bus_t genesis_bus = {
    .read8   = bus_read8,
    .read16  = bus_read16,
    .read32  = bus_read32,
    .write8  = bus_write8,
    .write16 = bus_write16,
    .write32 = bus_write32,
};

/* ---- Timing constants ---------------------------------------------------- */
#define NTSC_LINES              262
#define NTSC_VBLANK_LINE        224
#define CYCLES_PER_SCANLINE     488
#define Z80_CYCLES_PER_SCANLINE 228

/* ---- Init ---------------------------------------------------------------- */
int genesis_bare_init(const uint8_t *rom, size_t size)
{
    /* cpu_init() must come before bus_init(): cpu_init() zeros int_ack_fn,
     * while bus_init() calls cpu_set_int_ack(vdp_int_ack).  Reversing the
     * order would leave vdp_int_ack uncalled, causing ST_VINT to never clear
     * and the VBlank ISR to re-fire on every instruction after RTE. */
    cpu_init(CPU_MODEL_68000);
    if (bus_init(rom, size) < 0) {
        kprintf("genesis_bare_init: bus_init failed\n");
        return -1;
    }
    mem_set_bus(&genesis_bus);
    cpu_reset();
    return 0;
}

/* ---- Run N frames -------------------------------------------------------- */
void genesis_bare_run(int max_frames)
{
    for (int frame = 1; frame <= max_frames && !cpu.halted; frame++) {

        for (int line = 0; line < NTSC_LINES; line++) {

            /* 68k: run one scanline worth of cycles */
            int cycles_this_line = 0;
            while (cycles_this_line < CYCLES_PER_SCANLINE) {
                int c = cpu_step();
                if (c == 0) { cycles_this_line = CYCLES_PER_SCANLINE; break; }
                cpu.cycles += c;
                cycles_this_line += c;
            }

            /* Z80: run alongside 68k */
            if (line == NTSC_VBLANK_LINE)
                z80_assert_int();
            if (z80_is_running() && !io_z80_bus_held()) {
                int z_cycles = 0;
                while (z_cycles < Z80_CYCLES_PER_SCANLINE) {
                    int c = z80_step();
                    if (c == 0) break;
                    z_cycles += c;
                }
            }

            vdp_run_scanline(line);
        }
    }
}

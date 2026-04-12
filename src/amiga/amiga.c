/*
 * Amiga 500 system: system_t implementation + PAL run loop.
 *
 * Chapter 3: adds SDL2 graphical run loop (HAVE_SDL2).
 * Headless path (amiga_run_headless) unchanged from Chapter 2.
 *
 * PAL timing:
 *   CPU clock:  7,093,790 Hz
 *   Scanlines:  312 per frame, 50 Hz
 *   Cycles/line: 454 CPU cycles
 *   E-clock:    CPU / 10 → 45 E-clocks per scanline (rounded)
 */

#include "amiga.h"
#include "bus.h"
#include "cia.h"
#include "paula.h"
#include "cpu.h"
#include "memory.h"
#include <stdio.h>

#ifdef HAVE_SDL2
#  include "renderer.h"
#endif

/* ------------------------------------------------------------------ */
/*  PAL constants                                                       */
/* ------------------------------------------------------------------ */

#define PAL_LINES           312
#define CPU_CYCLES_PER_LINE 454
#define E_CLOCKS_PER_LINE    45   /* 454 / 10, rounded */

/* ------------------------------------------------------------------ */
/*  Bus vtable                                                          */
/* ------------------------------------------------------------------ */

static const mem_bus_t amiga_bus_vtable = {
    .read8   = amiga_bus_read8,
    .read16  = amiga_bus_read16,
    .read32  = amiga_bus_read32,
    .write8  = amiga_bus_write8,
    .write16 = amiga_bus_write16,
    .write32 = amiga_bus_write32,
};

/* ------------------------------------------------------------------ */
/*  Interrupt-acknowledge callback                                      */
/* ------------------------------------------------------------------ */

static void amiga_int_ack(int level)
{
    /*
     * Called when the CPU takes an autovector interrupt.
     * Clear the corresponding INTREQ bit(s) and re-evaluate.
     *
     * Naming note: hardware INTREQ is stored in amiga_paula.adkcon
     * (see paula.c for the full explanation of the offset mapping).
     */
    paula_t *p = &amiga_paula;
    switch (level) {
    case 1: p->adkcon &= (uint16_t)~(INTREQ_TBE | INTREQ_DSKBLK | INTREQ_SOFT); break;
    case 2: p->adkcon &= (uint16_t)~INTREQ_PORTS; break;
    case 3: p->adkcon &= (uint16_t)~(INTREQ_COPER | INTREQ_VERTB | INTREQ_BLIT); break;
    case 4: p->adkcon &= (uint16_t)~(INTREQ_AUD0 | INTREQ_AUD1 | INTREQ_AUD2 | INTREQ_AUD3); break;
    case 5: p->adkcon &= (uint16_t)~(INTREQ_RBF | INTREQ_DSKSYN); break;
    case 6: p->adkcon &= (uint16_t)~INTREQ_EXTER; break;
    default: break;
    }
    paula_update_irq(p);
}

/* ------------------------------------------------------------------ */
/*  system_t callbacks                                                  */
/* ------------------------------------------------------------------ */

static int amiga_init(const uint8_t *rom, size_t size)
{
    if (amiga_bus_init(rom, size) < 0)
        return -1;
    mem_set_bus(&amiga_bus_vtable);
    cpu_set_int_ack(amiga_int_ack);
    return 0;
}

static void amiga_reset(void)
{
    amiga_bus_reset();
}

static void amiga_shutdown(void)
{
    mem_set_bus(NULL);
    cpu_set_int_ack(NULL);
}

/* ------------------------------------------------------------------ */
/*  Run loop                                                            */
/* ------------------------------------------------------------------ */

void amiga_run(void)
{
#ifdef HAVE_SDL2
    if (renderer_init() < 0)
        return;

    /*
     * Framebuffer: AMIGA_HEIGHT visible lines × AMIGA_WIDTH pixels.
     * Lines 0–(AMIGA_HEIGHT-1) are rendered by Denise each frame.
     * Lines AMIGA_HEIGHT–311 still run the CPU and CIAs for correct timing
     * but are not blitted to the screen (vertical blanking / overscan region).
     */
    static uint32_t framebuffer[AMIGA_HEIGHT * AMIGA_WIDTH];

    while (!renderer_poll_events()) {
        for (int line = 0; line < PAL_LINES; line++) {
            /* Advance Agnus beam counter to the start of this scanline. */
            agnus_tick_scanline(&amiga_agnus, line);

            /* Run the 68000 for one scanline worth of CPU cycles. */
            int cycles = 0;
            while (cycles < CPU_CYCLES_PER_LINE) {
                int c = cpu_step();
                if (c == 0) {
                    cycles = CPU_CYCLES_PER_LINE;  /* CPU halted */
                    break;
                }
                cpu.cycles += (uint32_t)c;
                cycles += c;
            }

            /* Tick CIA-A (PORTS = level 2) and CIA-B (EXTER = level 6). */
            cia_tick(&amiga_cia_a, E_CLOCKS_PER_LINE, &amiga_paula, INTREQ_PORTS);
            cia_tick(&amiga_cia_b, E_CLOCKS_PER_LINE, &amiga_paula, INTREQ_EXTER);

            /* Render visible lines into the framebuffer. */
            if (line < AMIGA_HEIGHT) {
                denise_render_line(&amiga_denise,
                                   amiga_bus_chip_ram(),
                                   amiga_bus_chip_ram_size(),
                                   &framebuffer[line * AMIGA_WIDTH]);
            }
        }

        /* End of frame: fire vertical blank interrupt and present. */
        paula_assert_intreq(&amiga_paula, INTREQ_VERTB);
        renderer_present(framebuffer);
    }

    renderer_shutdown();
#else
    fprintf(stderr, "Amiga graphical mode requires SDL2.  Build with:  make amiga\n");
#endif
}

void amiga_run_headless(int max_frames)
{
    printf("Amiga headless: running %d PAL frames at 7.09 MHz\n", max_frames);

    uint32_t prev_pc = 0;

    for (int frame = 1; frame <= max_frames; frame++) {
        for (int line = 0; line < PAL_LINES; line++) {
            /* Run CPU for one scanline worth of cycles. */
            int cycles = 0;
            while (cycles < CPU_CYCLES_PER_LINE) {
                int c = cpu_step();
                if (c == 0) {
                    cycles = CPU_CYCLES_PER_LINE;  /* halted */
                    break;
                }
                cpu.cycles += (uint32_t)c;
                cycles += c;
            }

            /* Tick CIA-A (PORTS = level 2) and CIA-B (EXTER = level 6). */
            cia_tick(&amiga_cia_a, E_CLOCKS_PER_LINE, &amiga_paula, INTREQ_PORTS);
            cia_tick(&amiga_cia_b, E_CLOCKS_PER_LINE, &amiga_paula, INTREQ_EXTER);
        }

        /* End of frame: assert vertical blank interrupt (level 3). */
        paula_assert_intreq(&amiga_paula, INTREQ_VERTB);

        if (frame <= 5 || (frame <= 120 && cpu.pc != prev_pc)) {
            printf("Amiga frame %3d: PC=%06X SR=%04X D0=%08X A7=%08X%s\n",
                   frame, cpu.pc, cpu.sr, cpu.d[0], cpu.a[7],
                   cpu.halted ? " HALTED" : "");
        }
        prev_pc = cpu.pc;
    }

    printf("Amiga headless: done (%d frames)\n", max_frames);
}

/* ------------------------------------------------------------------ */
/*  system_t descriptor                                                 */
/* ------------------------------------------------------------------ */

const system_t system_amiga = {
    .name         = "amiga",
    .description  = "Commodore Amiga 500",
    .init         = amiga_init,
    .reset        = amiga_reset,
    .shutdown     = amiga_shutdown,
    .run          = amiga_run,
    .run_headless = amiga_run_headless,
    .bus          = &amiga_bus_vtable,
};

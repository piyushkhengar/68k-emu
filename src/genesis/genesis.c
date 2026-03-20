/*
 * Genesis system main loop.
 *
 * Runs the 68K CPU and VDP in lockstep at NTSC timing: 262 scanlines per
 * frame, ~488 68K cycles per scanline, 60 frames per second.  After each
 * frame the VDP framebuffer is presented via SDL2.
 *
 * When compiled without HAVE_SDL2, provides a stub that prints an error.
 */

#include "genesis.h"
#include "vdp.h"
#include "cpu.h"
#include "bus.h"
#include <stdio.h>

#define NTSC_LINES          262
#define CYCLES_PER_SCANLINE 488

#ifdef HAVE_SDL2
#include "renderer.h"

void genesis_run(void)
{
    if (renderer_init() < 0) {
        fprintf(stderr, "genesis: renderer init failed\n");
        return;
    }

    printf("Genesis: running (close window or press Escape to quit)\n");

    int quit = 0;
    int frame = 0;
    uint32_t prev_pc = 0;
    while (!quit) {
        for (int line = 0; line < NTSC_LINES && !quit; line++) {
            int cycles_this_line = 0;
            while (cycles_this_line < CYCLES_PER_SCANLINE) {
                int c = cpu_step();
                if (c == 0) {
                    cycles_this_line = CYCLES_PER_SCANLINE;
                    break;
                }
                cpu.cycles += c;
                cycles_this_line += c;
            }
            vdp_run_scanline(line);
        }

        frame++;
        int show = (frame <= 5) || (frame <= 120 && cpu.pc != prev_pc);
        if (show) {
            printf("Frame %3d: PC=%08X SR=%04X D0=%08X A7=%08X %s\n",
                   frame, cpu.pc, cpu.sr, cpu.d[0], cpu.a[7],
                   cpu.halted ? "HALTED" : "");
            printf("  VDP: R1=%02X R2=%02X R4=%02X R7=%02X R12=%02X "
                   "status=%04X CRAM[0]=%04X CRAM[1]=%04X\n",
                   vdp.regs[1], vdp.regs[2], vdp.regs[4],
                   vdp.regs[7], vdp.regs[12],
                   vdp.status, vdp.cram[0], vdp.cram[1]);
        }
        prev_pc = cpu.pc;

        renderer_present(vdp.framebuffer);
        quit = renderer_poll_events();
    }

    renderer_shutdown();
    printf("Genesis: exited\n");
}

#else /* !HAVE_SDL2 */

void genesis_run(void)
{
    fprintf(stderr, "Genesis mode requires SDL2.  Build with:  make genesis\n");
}

#endif /* HAVE_SDL2 */

void genesis_run_headless(int max_frames)
{
    printf("Genesis headless: running %d frames\n", max_frames);

    uint32_t prev_pc = 0;
    for (int frame = 1; frame <= max_frames; frame++) {
        for (int line = 0; line < 262; line++) {
            int cycles_this_line = 0;
            while (cycles_this_line < 488) {
                int c = cpu_step();
                if (c == 0) { cycles_this_line = 488; break; }
                cpu.cycles += c;
                cycles_this_line += c;
            }
            vdp_run_scanline(line);
        }

        int show = (frame <= 5) || (frame <= max_frames && cpu.pc != prev_pc);
        if (show) {
            printf("Frame %3d: PC=%08X SR=%04X D0=%08X A7=%08X %s\n",
                   frame, cpu.pc, cpu.sr, cpu.d[0], cpu.a[7],
                   cpu.halted ? "HALTED" : "");
            printf("  VDP: R1=%02X status=%04X CRAM=%04X,%04X,%04X,%04X  "
                   "mode=%02X vblflag=%02X ipl=%d\n",
                   vdp.regs[1], vdp.status,
                   vdp.cram[0], vdp.cram[1], vdp.cram[2], vdp.cram[3],
                   bus_read8(0xFFF600), bus_read8(0xFFF62A), cpu_ipl);
        }
        prev_pc = cpu.pc;
    }
    printf("Genesis headless: done\n");
}

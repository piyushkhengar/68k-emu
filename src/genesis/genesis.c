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
#include <stdio.h>

#ifdef HAVE_SDL2

#include "renderer.h"
#include "vdp.h"
#include "cpu.h"

#define NTSC_LINES          262
#define CYCLES_PER_SCANLINE 488

void genesis_run(void)
{
    if (renderer_init() < 0) {
        fprintf(stderr, "genesis: renderer init failed\n");
        return;
    }

    printf("Genesis: running (close window or press Escape to quit)\n");

    int quit = 0;
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

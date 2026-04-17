/*
 * SDL2 renderer for the Amiga 500 emulator.
 *
 * Chapter 3 scope: display a 320×256 PAL framebuffer in a 640×512 window
 * (2× nearest-neighbour scaling).  No input handling — that is added in
 * Chapter 5 alongside keyboard and mouse support.
 */

#ifndef AMIGA_RENDERER_H
#define AMIGA_RENDERER_H

#include <stdint.h>

#define AMIGA_WIDTH  640   /* framebuffer pixel columns (HIRES native) */
#define AMIGA_HEIGHT 256   /* visible PAL lines (of 312)   */

/* Open the SDL2 window.  Returns 0 on success, -1 on failure. */
int  renderer_init(void);

/* Destroy the window and shut down SDL2. */
void renderer_shutdown(void);

/*
 * Upload the framebuffer to the GPU and present it.
 * framebuffer must be AMIGA_WIDTH × AMIGA_HEIGHT ARGB8888 pixels,
 * row-major (row 0 = top of screen).
 */
void renderer_present(const uint32_t *framebuffer);

/*
 * Drain the SDL event queue.
 * Returns: 0 = continue, 1 = quit requested, 2 = Amiga reset requested.
 * Reset is triggered by Ctrl + Left Amiga + Right Amiga
 * (mapped to Ctrl + Left Alt/Option + Right Alt/Option on the host;
 *  GUI/Cmd/Win keys also accepted for non-macOS platforms).
 */
int  renderer_poll_events(void);

#endif /* AMIGA_RENDERER_H */

#ifndef GENESIS_RENDERER_H
#define GENESIS_RENDERER_H

#include <stdint.h>

/*
 * SDL2-based renderer for the Genesis VDP.
 *
 * Manages the SDL window, renderer, and texture.  The framebuffer is a
 * 320x224 array of ARGB8888 pixels written by the VDP each scanline and
 * presented to the screen once per frame.
 */

#define GEN_WIDTH   320
#define GEN_HEIGHT  224

int  renderer_init(void);
void renderer_shutdown(void);

/* Upload the framebuffer to the GPU texture and present it. */
void renderer_present(const uint32_t *framebuffer);

/* Process SDL events.  Returns 0 normally, 1 if the user requested quit. */
int  renderer_poll_events(void);

#endif /* GENESIS_RENDERER_H */

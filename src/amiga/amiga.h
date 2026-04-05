#ifndef AMIGA_AMIGA_H
#define AMIGA_AMIGA_H

#include "../system.h"

/*
 * Amiga 500 system — ties the 68000 CPU, CIA-A/B, Paula, and bus together
 * into a per-scanline main loop running at 50 Hz PAL.
 *
 * Use system_amiga with the system_t interface.
 * Graphical mode (amiga_run) requires SDL2; build with `make amiga`.
 * Headless mode (amiga_run_headless) works without SDL2.
 */

extern const system_t system_amiga;

void amiga_run(void);
void amiga_run_headless(int max_frames);

#endif /* AMIGA_AMIGA_H */

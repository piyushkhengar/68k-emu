#ifndef GENESIS_GENESIS_H
#define GENESIS_GENESIS_H

#include "../system.h"

/*
 * Genesis system -- ties the 68K CPU, VDP, bus, and SDL renderer together
 * into a per-scanline main loop running at 60 Hz NTSC.
 *
 * Use system_genesis with the system_t interface, or call the raw functions
 * directly if needed.
 */

extern const system_t system_genesis;

void genesis_run(void);
void genesis_run_headless(int max_frames);

#endif /* GENESIS_GENESIS_H */

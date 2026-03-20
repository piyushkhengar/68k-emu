#ifndef GENESIS_GENESIS_H
#define GENESIS_GENESIS_H

/*
 * Genesis system -- ties the 68K CPU, VDP, bus, and SDL renderer together
 * into a per-scanline main loop running at 60 Hz NTSC.
 *
 * Call genesis_run() after bus_init() and cpu_reset().  It does not return
 * until the user closes the window or presses Escape.
 */

void genesis_run(void);
void genesis_run_headless(int max_frames);

#endif /* GENESIS_GENESIS_H */

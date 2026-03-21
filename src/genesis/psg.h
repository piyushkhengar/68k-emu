#ifndef GENESIS_PSG_H
#define GENESIS_PSG_H

#include <stdint.h>

/*
 * SN76489 PSG (Programmable Sound Generator) emulator.
 *
 * 3 square-wave tone channels + 1 noise channel, each with 4-bit
 * volume attenuation.  Uses a latch/data register protocol.
 *
 * The PSG is clocked at ~3.58 MHz (Z80 clock) and its internal
 * divider produces output at clock/16 ≈ 223 kHz, which is
 * downsampled to the host audio rate during sample generation.
 */

void psg_init(void);
void psg_reset(void);

/* Write a byte via the SN76489 latch/data protocol. */
void psg_write(uint8_t val);

/* Generate 'count' stereo sample pairs (interleaved L,R) at the
 * given sample rate, mixed additively into 'buf' (int32_t to
 * avoid overflow during mixing). */
void psg_run_samples(int32_t *buf, int count, int sample_rate);

#endif /* GENESIS_PSG_H */

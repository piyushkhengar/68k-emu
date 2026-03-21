#ifndef GENESIS_YM2612_H
#define GENESIS_YM2612_H

#include <stdint.h>

/*
 * YM2612 (OPN2) FM synthesis chip emulator.
 *
 * 6 channels of 4-operator FM synthesis, plus a DAC mode on channel 6
 * that plays raw 8-bit PCM samples.  Stereo output with per-channel
 * left/right panning.
 *
 * Port mapping (Z80 side):
 *   $4000  Part I address
 *   $4001  Part I data
 *   $4002  Part II address
 *   $4003  Part II data
 */

void    ym2612_init(void);
void    ym2612_reset(void);
void    ym2612_write(uint8_t port, uint8_t val);
uint8_t ym2612_read(void);

/* Generate 'count' stereo sample pairs, mixed additively into buf
 * (int32_t to avoid overflow during mixing). */
void    ym2612_run_samples(int32_t *buf, int count, int sample_rate);

#endif /* GENESIS_YM2612_H */

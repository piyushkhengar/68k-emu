#ifndef GENESIS_AUDIO_H
#define GENESIS_AUDIO_H

/*
 * SDL2 audio backend for the Genesis emulator.
 *
 * Opens an audio device at 44100 Hz, 16-bit signed stereo.
 *
 * Audio is generated incrementally per scanline rather than all at once
 * per frame, so that register changes by the Z80/68K during the frame
 * are reflected at the correct point in the audio stream.  This is
 * essential for DAC playback quality and accurate PSG/FM timing.
 */

#define AUDIO_SAMPLE_RATE 44100
#define AUDIO_SAMPLES_PER_FRAME 735   /* 44100 / 60 */

int  audio_init(void);
void audio_shutdown(void);

/* Generate the proportional audio samples for one scanline.
 * Call this once per scanline from the main emulation loop. */
void audio_run_scanline(void);

/* Clamp the accumulated frame buffer and queue it for playback.
 * Call this once per frame after all scanlines have been processed. */
void audio_push_frame(void);

#endif /* GENESIS_AUDIO_H */

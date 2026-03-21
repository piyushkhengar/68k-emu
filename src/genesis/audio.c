/*
 * SDL2 audio backend for the Genesis emulator.
 *
 * Audio is generated per-scanline (262 scanlines/frame) rather than
 * once per frame, so that PSG/YM2612 register changes are reflected
 * at the correct time in the audio stream.  This is particularly
 * important for YM2612 DAC playback, which streams 8-bit PCM samples
 * via register $2A at ~8 kHz.
 *
 * All mixing is done in int32_t to avoid overflow, then clamped to
 * int16_t before queuing for SDL playback.
 */

#include "audio.h"
#include "psg.h"
#include "ym2612.h"
#include <SDL.h>
#include <string.h>
#include <stdio.h>

#define NTSC_LINES 262

/* 16.16 fixed-point: samples per scanline = 735 * 65536 / 262 */
#define SAMPLES_PER_SCANLINE_FP \
    ((uint32_t)(((uint64_t)AUDIO_SAMPLES_PER_FRAME << 16) / NTSC_LINES))

static SDL_AudioDeviceID audio_dev;

static int32_t  frame_buf[AUDIO_SAMPLES_PER_FRAME * 2];
static int      frame_pos;       /* stereo samples generated so far */
static uint32_t scanline_frac;   /* fractional accumulator */

int audio_init(void)
{
    SDL_AudioSpec want, have;
    memset(&want, 0, sizeof(want));
    want.freq     = AUDIO_SAMPLE_RATE;
    want.format   = AUDIO_S16SYS;
    want.channels = 2;
    want.samples  = 1024;

    audio_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (!audio_dev) {
        fprintf(stderr, "SDL_OpenAudioDevice failed: %s\n", SDL_GetError());
        return -1;
    }

    SDL_PauseAudioDevice(audio_dev, 0);
    memset(frame_buf, 0, sizeof(frame_buf));
    frame_pos = 0;
    scanline_frac = 0;
    return 0;
}

void audio_shutdown(void)
{
    if (audio_dev) {
        SDL_CloseAudioDevice(audio_dev);
        audio_dev = 0;
    }
}

void audio_run_scanline(void)
{
    if (!audio_dev) return;

    scanline_frac += SAMPLES_PER_SCANLINE_FP;
    int target = scanline_frac >> 16;
    int to_gen = target - frame_pos;
    if (to_gen <= 0) return;
    if (frame_pos + to_gen > AUDIO_SAMPLES_PER_FRAME)
        to_gen = AUDIO_SAMPLES_PER_FRAME - frame_pos;
    if (to_gen <= 0) return;

    int32_t *p = &frame_buf[frame_pos * 2];
    psg_run_samples(p, to_gen, AUDIO_SAMPLE_RATE);
    ym2612_run_samples(p, to_gen, AUDIO_SAMPLE_RATE);
    frame_pos += to_gen;
}

void audio_push_frame(void)
{
    if (!audio_dev) return;

    /* Generate any remaining samples to fill the frame */
    int remaining = AUDIO_SAMPLES_PER_FRAME - frame_pos;
    if (remaining > 0) {
        int32_t *p = &frame_buf[frame_pos * 2];
        psg_run_samples(p, remaining, AUDIO_SAMPLE_RATE);
        ym2612_run_samples(p, remaining, AUDIO_SAMPLE_RATE);
    }

    /* Clamp int32 -> int16 */
    int16_t out[AUDIO_SAMPLES_PER_FRAME * 2];
    for (int i = 0; i < AUDIO_SAMPLES_PER_FRAME * 2; i++) {
        int32_t s = frame_buf[i];
        if (s > 32767) s = 32767;
        else if (s < -32768) s = -32768;
        out[i] = (int16_t)s;
    }

    /* Throttle: don't let the queue grow unbounded */
    while (SDL_GetQueuedAudioSize(audio_dev) > AUDIO_SAMPLE_RATE * 4)
        SDL_Delay(1);

    SDL_QueueAudio(audio_dev, out, sizeof(out));

    /* Reset for next frame */
    memset(frame_buf, 0, sizeof(frame_buf));
    frame_pos = 0;
    scanline_frac &= 0xFFFF;  /* keep fractional remainder */
}

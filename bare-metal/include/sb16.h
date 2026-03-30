#ifndef SB16_H
#define SB16_H

/*
 * bare-metal/include/sb16.h
 * Sound Blaster 16 driver — polling (no IRQ) version.
 *
 * Requires -device sb16 in the QEMU command line (or real SB16 hardware).
 * No IDT, PIC remapping, or x86 interrupts are used.
 */

#define SB16_SAMPLE_RATE 44100u

/* Detect and initialise the SB16 DSP + DMA channel 5.
 * Returns 1 on success, 0 if no SB16 is present. */
int sb16_init(void);

/* Returns 1 if sb16_init() succeeded. */
int sb16_available(void);

/* Call once per video frame.  Polls DMA position and fills the inactive
 * half-buffer with YM2612 + PSG audio.  Returns immediately if the DMA
 * has not yet advanced past the half we filled last time. */
void sb16_render_frame(void);

#endif /* SB16_H */

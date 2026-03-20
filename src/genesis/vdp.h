#ifndef GENESIS_VDP_H
#define GENESIS_VDP_H

#include <stdint.h>

/*
 * Genesis VDP (Video Display Processor) -- YM7101 / 315-5313
 *
 * Accessible to the 68K at 0xC00000-0xDFFFFF (mirrored every 32 bytes):
 *   0xC00000-03  Data port (read/write VRAM, CRAM, VSRAM)
 *   0xC00004-07  Control port (write: register set / address setup; read: status)
 *   0xC00008-0F  H/V counter (read-only)
 *   0xC00010-17  PSG (write-only, stubbed)
 *
 * The VDP has its own internal address bus and three private memories:
 *   VRAM  -- 64 KB for patterns, nametables, sprite table
 *   CRAM  -- 64 entries of 9-bit BGR colour (format: 0BBB0GGG0RRR0)
 *   VSRAM -- 40 entries of 11-bit vertical scroll values
 *
 * Status register (active-high):
 *   Bit 0  PAL flag            Bit 5  Sprite collision
 *   Bit 1  DMA busy            Bit 6  Sprite overflow
 *   Bit 2  HBlank              Bit 7  Vertical-interrupt pending
 *   Bit 3  VBlank              Bit 8  FIFO full
 *   Bit 4  Odd frame           Bit 9  FIFO empty
 */

typedef struct {
    uint8_t  regs[24];          /* VDP registers 0-23 */
    uint8_t  vram[0x10000];     /* 64 KB video RAM */
    uint16_t cram[64];          /* 64 colour entries */
    uint16_t vsram[40];         /* 40 vertical-scroll entries */

    uint16_t status;            /* Status register */
    uint16_t control_latch;     /* Saved first control word */
    int      control_pending;   /* Waiting for second control word? */
    uint8_t  code;              /* 6-bit access type + DMA flag */
    uint32_t addr;              /* Current VRAM/CRAM/VSRAM address */

    int      dma_fill_pending;  /* VRAM-fill awaiting data port write */
    uint16_t read_buffer;       /* Pre-fetch buffer for data port reads */

    int      line;              /* Current scanline (0-261 NTSC) */
    int      hcounter;          /* Horizontal pixel position */

    int      hint_counter;      /* HBlank interrupt countdown (loaded from reg 10) */
    int      hint_pending;      /* HBlank interrupt line asserted */

    uint32_t framebuffer[320 * 224]; /* ARGB8888 output (filled per-scanline) */
} vdp_t;

extern vdp_t vdp;

void     vdp_init(void);
void     vdp_reset(void);

/* Port access -- called from the bus layer */
uint16_t vdp_data_read(void);
void     vdp_data_write(uint16_t val);
uint16_t vdp_control_read(void);
void     vdp_control_write(uint16_t val);
uint16_t vdp_hv_read(void);

/* Scanline processing -- called once per scanline from the genesis main loop.
 * Manages VBlank/HBlank flags, HInt counter, and drives cpu_ipl. */
void     vdp_run_scanline(int line);

/* Called when the 68K acknowledges a VDP interrupt (level 4 or 6).
 * Clears the corresponding pending flag so the interrupt doesn't
 * re-fire until the next VBlank/HBlank edge. */
void     vdp_int_ack(int level);

#endif /* GENESIS_VDP_H */

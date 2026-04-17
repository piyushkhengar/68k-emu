/*
 * Agnus — Amiga 500 DMA controller, Copper, Blitter, and beam counter.
 *
 * Chapter 3 step 1: DMA control register + PAL beam counters.
 * Chapter 3 step 3: Copper (MOVE/WAIT) + synchronous Blitter.
 *
 * Register ownership (offsets from 0xDFF000 base):
 *   Read:
 *     0x002  DMACONR   DMA control (read mirror); bit 14 = BBUSY (0=idle, 1=busy)
 *     0x004  VPOSR     Beam high: bit 0 = line[8]
 *     0x006  VHPOSR    Beam low:  bits 15:8 = line[7:0], bits 7:0 = hpos[7:0]
 *   Write (SET/CLR: bit 15 = 1 → set, 0 → clear):
 *     0x096  DMACON    DMA enable flags
 *   Write (plain):
 *     0x040  BLTCON0   Blitter control 0 (minterm + channel enables + A shift)
 *     0x042  BLTCON1   Blitter control 1 (B shift)
 *     0x044  BLTAFWM   Blitter first-word mask for A
 *     0x046  BLTALWM   Blitter last-word mask for A
 *     0x048  BLTCPTH   Blitter C pointer high (bits 20:16)
 *     0x04A  BLTCPTL   Blitter C pointer low  (bits 15:1)
 *     0x04C  BLTBPTH   Blitter B pointer high
 *     0x04E  BLTBPTL   Blitter B pointer low
 *     0x050  BLTAPTH   Blitter A pointer high
 *     0x052  BLTAPTL   Blitter A pointer low
 *     0x054  BLTDPTH   Blitter D pointer high
 *     0x056  BLTDPTL   Blitter D pointer low
 *     0x058  BLTSIZE   Blitter size + trigger (bits 15:6 = height, 5:0 = width)
 *     0x060  BLTCMOD   Blitter C row modulo (signed bytes)
 *     0x062  BLTBMOD   Blitter B row modulo
 *     0x064  BLTAMOD   Blitter A row modulo
 *     0x066  BLTDMOD   Blitter D row modulo
 *     0x080  COP1LCH   Copper list 1 pointer high (bits 20:16)
 *     0x082  COP1LCL   Copper list 1 pointer low
 *     0x084  COP2LCH   Copper list 2 pointer high
 *     0x086  COP2LCL   Copper list 2 pointer low
 *     0x088  COPJMP1   Strobe: restart Copper from COP1LC
 *     0x08A  COPJMP2   Strobe: restart Copper from COP2LC
 *
 * DMACON bit constants (Agnus-owned subset):
 *   Bit 9  DMAEN — master DMA enable (must be set for any DMA to run)
 *   Bit 8  BPLEN — bitplane DMA enable
 *   Bit 7  COPEN — Copper DMA enable
 *   Bit 6  BLTEN — Blitter DMA enable
 *   Bit 5  SPREN — sprite DMA enable
 *   Bit 4  DSKEN — disk DMA enable
 *   Bits 3:0 — audio channels 3:0 (Paula-owned; Agnus mirrors them in DMACONR)
 *
 * PAL beam:
 *   312 scanlines per frame, 454 CPU cycles per scanline.
 *
 * Copper instruction set:
 *   MOVE  IR1[0]=0  IR1[8:1]=reg_offset/2  IR2=value
 *   WAIT  IR1[0]=1  IR1[15:8]=VP  IR1[7:1]=HP  IR2[15:8]=BFV  IR2[7:1]=BFH  IR2[0]=0
 *   SKIP  IR1[0]=1  same as WAIT but IR2[0]=1 — skip next instr if condition met
 *   End:  IR1=0xFFFF  IR2=0xFFFE
 *
 * Blitter BLTCON0:
 *   Bits 15:12 = ASH (A shift amount 0–15)
 *   Bit 11 = USEA, Bit 10 = USEB, Bit 9 = USEC, Bit 8 = USED
 *   Bits 7:0  = LF (8-bit minterm: bit N = output when {a,b,c}=N)
 */

#ifndef AMIGA_AGNUS_H
#define AMIGA_AGNUS_H

#include <stdint.h>

/* DMACON bits (Agnus-owned) */
#define DMACON_DMAEN  (1u << 9)   /* master DMA enable */
#define DMACON_BPLEN  (1u << 8)   /* bitplane DMA */
#define DMACON_COPEN  (1u << 7)   /* Copper DMA */
#define DMACON_BLTEN  (1u << 6)   /* Blitter DMA */
#define DMACON_SPREN  (1u << 5)   /* sprite DMA */
#define DMACON_DSKEN  (1u << 4)   /* disk DMA */

typedef struct {
    /* Beam counters (step 1) */
    uint16_t dmacon;     /* accumulated DMA control state                 */
    int      line;       /* current PAL scanline 0–311                    */
    int      hpos;       /* horizontal beam position 0–453                */

    /* Copper (step 3) */
    uint32_t cop1lc;     /* Copper list 1 base address in chip RAM        */
    uint32_t cop2lc;     /* Copper list 2 base address in chip RAM        */
    uint32_t copper_pc;  /* current Copper fetch address                  */

    /* Blitter (step 3) */
    uint16_t bltcon0;    /* minterm + channel enables + A shift           */
    uint16_t bltcon1;    /* B shift                                       */
    uint32_t bltapt;     /* channel A source pointer                      */
    uint32_t bltbpt;     /* channel B source pointer                      */
    uint32_t bltcpt;     /* channel C source pointer                      */
    uint32_t bltdpt;     /* destination D pointer                         */
    uint16_t bltafwm;    /* first-word mask applied to A                  */
    uint16_t bltalwm;    /* last-word mask applied to A                   */
    int16_t  bltamod;    /* row modulo for A (signed bytes)               */
    int16_t  bltbmod;    /* row modulo for B                              */
    int16_t  bltcmod;    /* row modulo for C                              */
    int16_t  bltdmod;    /* row modulo for D                              */
    uint16_t bltadat;    /* A data register (line mode texture)           */
    uint16_t bltbdat;    /* B data register (line mode pattern)           */
    uint16_t bltcdat;    /* C data register                               */

    /* Bitplane DMA control (step 4 — display setup) */
    uint16_t ddfstrt;   /* data-fetch start position (0x092)             */
    uint16_t ddfstop;   /* data-fetch stop position  (0x094)             */
    int16_t  bpl1mod;   /* odd  bitplane modulo (0x108, signed)          */
    int16_t  bpl2mod;   /* even bitplane modulo (0x10A, signed)          */

    /* Display window (viewport) */
    uint16_t diwstrt;   /* display window start (0x08E): V[15:8] H[7:0] */
    uint16_t diwstop;   /* display window stop  (0x090): V[15:8] H[7:0] */
} agnus_t;

/* Lifecycle */
void agnus_init(agnus_t *ag);
void agnus_reset(agnus_t *ag);

/*
 * agnus_tick_scanline — advance beam to the start of the given scanline.
 * Sets ag->line = line and resets ag->hpos = 0.
 */
void agnus_tick_scanline(agnus_t *ag, int line);

/*
 * Register read/write (offset from 0xDFF000).
 * agnus_write_reg does NOT trigger the Blitter on BLTSIZE — that is
 * done by bus.c so it has access to chip_ram.
 */
uint16_t agnus_read_reg(const agnus_t *ag, uint16_t offset);
void     agnus_write_reg(agnus_t *ag, uint16_t offset, uint16_t val);

/*
 * Callback type for Copper MOVE writes — called with (register_offset, value).
 * In production use amiga_bus_write_custom() from bus.h.
 */
typedef void (*agnus_write_fn)(uint16_t offset, uint16_t val);

/*
 * agnus_copper_scanline — execute Copper instructions for the current scanline.
 *
 * Reads instructions from chip_ram starting at ag->copper_pc.
 * MOVE instructions call write_reg(offset, val).
 * Stops at a WAIT for a future beam position or the end sentinel.
 * Call once per scanline, after agnus_tick_scanline().
 */
void agnus_copper_scanline(agnus_t *ag,
                           const uint8_t *chip_ram, uint32_t chip_ram_size,
                           agnus_write_fn write_reg);

/*
 * agnus_blitter_execute — run the pending blit synchronously on chip_ram.
 *
 * Called by bus.c when BLTSIZE is written (which triggers the blit).
 * chip_ram must be mutable — the Blitter writes to it.
 */
/*
 * bltsize — the value written to BLTSIZE (bits 15:6 = height, 5:0 = width).
 * bus.c passes this directly because it has the value at the point of write.
 */
void agnus_blitter_execute(agnus_t *ag,
                           uint8_t *chip_ram, uint32_t chip_ram_size,
                           uint16_t bltsize);

#endif /* AMIGA_AGNUS_H */

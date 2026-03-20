/*
 * Genesis VDP foundation.
 *
 * Implements the register interface, VRAM/CRAM/VSRAM storage, the two-word
 * control-port protocol, data-port read/write with auto-increment, and the
 * three DMA modes (68K-to-VDP, VRAM fill, VRAM copy) as immediate operations.
 *
 * Scanline timing (VBlank / HBlank toggling) is not yet wired -- the status
 * register starts with VBlank set so boot code that polls for it won't hang.
 */

#include "vdp.h"
#include "bus.h"
#include <string.h>

/* ------------------------------------------------------------------ */
/*  Global VDP state                                                   */
/* ------------------------------------------------------------------ */

vdp_t vdp;

/* ------------------------------------------------------------------ */
/*  Status-register bit masks                                          */
/* ------------------------------------------------------------------ */

#define ST_PAL        (1u << 0)
#define ST_DMA        (1u << 1)
#define ST_HBLANK     (1u << 2)
#define ST_VBLANK     (1u << 3)
#define ST_ODD        (1u << 4)
#define ST_SPR_COL    (1u << 5)
#define ST_SPR_OVF    (1u << 6)
#define ST_VINT       (1u << 7)
#define ST_FIFO_FULL  (1u << 8)
#define ST_FIFO_EMPTY (1u << 9)

/* ------------------------------------------------------------------ */
/*  Init / Reset                                                       */
/* ------------------------------------------------------------------ */

void vdp_init(void)
{
    memset(&vdp, 0, sizeof(vdp));
    vdp.status = ST_VBLANK | ST_FIFO_EMPTY;
}

void vdp_reset(void)
{
    vdp_init();
}

/* ------------------------------------------------------------------ */
/*  Helpers                                                            */
/* ------------------------------------------------------------------ */

static void addr_inc(void)
{
    vdp.addr = (vdp.addr + vdp.regs[15]) & 0xFFFF;
}

static void prefetch(void)
{
    switch (vdp.code & 0x0F) {
    case 0x00: /* VRAM read */
        vdp.read_buffer =
            ((uint16_t)vdp.vram[vdp.addr & 0xFFFF] << 8) |
            vdp.vram[(vdp.addr + 1) & 0xFFFF];
        break;
    case 0x08: /* CRAM read */
        vdp.read_buffer = vdp.cram[(vdp.addr >> 1) & 0x3F];
        break;
    case 0x04: /* VSRAM read */
        if ((vdp.addr >> 1) < 40)
            vdp.read_buffer = vdp.vsram[vdp.addr >> 1];
        else
            vdp.read_buffer = 0;
        break;
    default:
        vdp.read_buffer = 0;
        break;
    }
}

/* ------------------------------------------------------------------ */
/*  DMA                                                                */
/* ------------------------------------------------------------------ */

static void dma_68k_to_vdp(void)
{
    uint16_t len = vdp.regs[19] | ((uint16_t)vdp.regs[20] << 8);
    if (len == 0)
        len = 0xFFFF;

    uint32_t src = ((uint32_t)vdp.regs[21]            |
                    ((uint32_t)vdp.regs[22] << 8)      |
                    ((uint32_t)(vdp.regs[23] & 0x7F) << 16)) << 1;

    uint8_t target = vdp.code & 0x07;

    for (uint16_t i = 0; i < len; i++) {
        uint16_t val = bus_read16(src & 0xFFFFFF);

        switch (target) {
        case 0x01: /* VRAM */
            vdp.vram[vdp.addr & 0xFFFF] = val >> 8;
            vdp.vram[(vdp.addr + 1) & 0xFFFF] = val & 0xFF;
            break;
        case 0x03: /* CRAM */
            vdp.cram[(vdp.addr >> 1) & 0x3F] = val & 0x0EEE;
            break;
        case 0x05: /* VSRAM */
            if ((vdp.addr >> 1) < 40)
                vdp.vsram[vdp.addr >> 1] = val & 0x07FF;
            break;
        }

        src += 2;
        addr_inc();
    }

    src >>= 1;
    vdp.regs[21] = src & 0xFF;
    vdp.regs[22] = (src >> 8) & 0xFF;
    vdp.regs[23] = (vdp.regs[23] & 0x80) | ((src >> 16) & 0x7F);
    vdp.regs[19] = 0;
    vdp.regs[20] = 0;
}

/*
 * VRAM fill: the low byte of the written word goes to the initial address,
 * then the high byte is replicated for the remaining (length-1) writes.
 */
static void dma_vram_fill(uint16_t val)
{
    uint16_t len = vdp.regs[19] | ((uint16_t)vdp.regs[20] << 8);
    if (len == 0)
        len = 0xFFFF;

    uint8_t fill = val >> 8;

    vdp.vram[vdp.addr & 0xFFFF] = val & 0xFF;
    addr_inc();

    for (uint16_t i = 1; i < len; i++) {
        vdp.vram[vdp.addr & 0xFFFF] = fill;
        addr_inc();
    }

    vdp.regs[19] = 0;
    vdp.regs[20] = 0;
    vdp.dma_fill_pending = 0;
}

static void dma_vram_copy(void)
{
    uint16_t len = vdp.regs[19] | ((uint16_t)vdp.regs[20] << 8);
    if (len == 0)
        len = 0xFFFF;

    uint16_t src = vdp.regs[21] | ((uint16_t)vdp.regs[22] << 8);

    for (uint16_t i = 0; i < len; i++) {
        vdp.vram[vdp.addr & 0xFFFF] = vdp.vram[src & 0xFFFF];
        src++;
        addr_inc();
    }

    vdp.regs[21] = src & 0xFF;
    vdp.regs[22] = (src >> 8) & 0xFF;
    vdp.regs[19] = 0;
    vdp.regs[20] = 0;
}

static void dma_exec(void)
{
    switch (vdp.regs[23] >> 6) {
    case 0: case 1:     /* 68K -> VDP */
        dma_68k_to_vdp();
        break;
    case 2:             /* VRAM fill (deferred to next data-port write) */
        vdp.dma_fill_pending = 1;
        break;
    case 3:             /* VRAM copy */
        dma_vram_copy();
        break;
    }
}

/* ------------------------------------------------------------------ */
/*  Control port                                                       */
/* ------------------------------------------------------------------ */

void vdp_control_write(uint16_t val)
{
    /*
     * Register write: 10RRRRR_DDDDDDDD  (only when not pending second word).
     * A word with bits 15-14 == 10 while control_pending==0 is always a
     * register set, not the first word of a two-word address command.
     */
    if ((val & 0xC000) == 0x8000 && !vdp.control_pending) {
        uint8_t reg  = (val >> 8) & 0x1F;
        if (reg < 24)
            vdp.regs[reg] = val & 0xFF;
        vdp.code = (vdp.code & 0x3C) | ((val >> 14) & 0x03);
        vdp.addr = (vdp.addr & 0xC000) | (val & 0x3FFF);
        return;
    }

    if (!vdp.control_pending) {
        /* First word of two-word address / code setup */
        vdp.control_latch = val;
        vdp.addr = (vdp.addr & 0xC000) | (val & 0x3FFF);
        vdp.code = (vdp.code & 0x3C) | ((val >> 14) & 0x03);
        vdp.control_pending = 1;
    } else {
        /* Second word -- completes the address and code */
        vdp.addr = (vdp.addr & 0x3FFF) | ((val & 0x03) << 14);
        vdp.code = (vdp.code & 0x03) | ((val >> 2) & 0x3C);
        vdp.control_pending = 0;

        if ((vdp.code & 0x20) && (vdp.regs[1] & 0x10))
            dma_exec();

        /* Pre-fetch for read commands (code bit 0 == 0, not DMA) */
        if (!(vdp.code & 0x01) && !(vdp.code & 0x20)) {
            prefetch();
            addr_inc();
        }
    }
}

uint16_t vdp_control_read(void)
{
    vdp.control_pending = 0;

    uint16_t s = vdp.status;

    /* Reading clears the one-shot flags */
    vdp.status &= ~(ST_VINT | ST_SPR_OVF | ST_SPR_COL);

    return s;
}

/* ------------------------------------------------------------------ */
/*  Data port                                                          */
/* ------------------------------------------------------------------ */

void vdp_data_write(uint16_t val)
{
    vdp.control_pending = 0;

    if (vdp.dma_fill_pending) {
        dma_vram_fill(val);
        return;
    }

    switch (vdp.code & 0x07) {
    case 0x01: /* VRAM */
        vdp.vram[vdp.addr & 0xFFFF] = val >> 8;
        vdp.vram[(vdp.addr + 1) & 0xFFFF] = val & 0xFF;
        break;
    case 0x03: /* CRAM */
        vdp.cram[(vdp.addr >> 1) & 0x3F] = val & 0x0EEE;
        break;
    case 0x05: /* VSRAM */
        if ((vdp.addr >> 1) < 40)
            vdp.vsram[vdp.addr >> 1] = val & 0x07FF;
        break;
    }

    addr_inc();
}

uint16_t vdp_data_read(void)
{
    vdp.control_pending = 0;

    uint16_t val = vdp.read_buffer;
    prefetch();
    addr_inc();
    return val;
}

/* ------------------------------------------------------------------ */
/*  H/V counter                                                        */
/* ------------------------------------------------------------------ */

uint16_t vdp_hv_read(void)
{
    uint8_t v = (uint8_t)(vdp.line & 0xFF);
    uint8_t h = (uint8_t)((vdp.hcounter >> 1) & 0xFF);
    return ((uint16_t)v << 8) | h;
}

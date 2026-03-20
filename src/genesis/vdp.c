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
#include "cpu.h"
#include <string.h>

static void vdp_update_ipl(void);

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
        if (reg == 0 || reg == 1)
            vdp_update_ipl();
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

    vdp_update_ipl();

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

/* ------------------------------------------------------------------ */
/*  Interrupt priority                                                 */
/* ------------------------------------------------------------------ */

static void vdp_update_ipl(void)
{
    int level = 0;
    if (vdp.hint_pending && (vdp.regs[0] & 0x10))
        level = 4;
    if ((vdp.status & ST_VINT) && (vdp.regs[1] & 0x20))
        level = 6;
    cpu_ipl = level;
}

void vdp_int_ack(int level)
{
    if (level == 6)
        vdp.status &= ~ST_VINT;
    else if (level == 4)
        vdp.hint_pending = 0;
    vdp_update_ipl();
}

/* ------------------------------------------------------------------ */
/*  Colour conversion                                                  */
/* ------------------------------------------------------------------ */

/* Convert Genesis 9-bit BGR (0BBB0GGG0RRR0) to ARGB8888. */
static uint32_t cram_to_argb(uint16_t c)
{
    uint8_t r = (uint8_t)(((c >>  1) & 7) * 255 / 7);
    uint8_t g = (uint8_t)(((c >>  5) & 7) * 255 / 7);
    uint8_t b = (uint8_t)(((c >>  9) & 7) * 255 / 7);
    return 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

/* ------------------------------------------------------------------ */
/*  Tile and plane rendering                                           */
/* ------------------------------------------------------------------ */

#define NTSC_LINES      262
#define ACTIVE_LINES    224
#define SCREEN_WIDTH    320

static void get_plane_size(int *cols, int *rows)
{
    static const int tbl[] = { 32, 64, 32, 128 };
    *cols = tbl[vdp.regs[16] & 0x03];
    *rows = tbl[(vdp.regs[16] >> 4) & 0x03];
}

static int get_hscroll(int line, int plane_b)
{
    uint16_t base = (uint16_t)(vdp.regs[13] & 0x3F) << 10;
    int offset;
    switch (vdp.regs[11] & 0x03) {
    case 2:  offset = (line & ~7) * 4; break;
    case 3:  offset = line * 4; break;
    default: offset = 0; break;
    }
    uint16_t addr = (base + offset + (plane_b ? 2 : 0)) & 0xFFFF;
    return ((vdp.vram[addr] << 8) | vdp.vram[(addr + 1) & 0xFFFF]) & 0x3FF;
}

static int get_vscroll(int column, int plane_b)
{
    if (vdp.regs[11] & 0x04) {
        int idx = (column >> 4) * 2 + (plane_b ? 1 : 0);
        return (idx < 40) ? (vdp.vsram[idx] & 0x7FF) : 0;
    }
    return vdp.vsram[plane_b ? 1 : 0] & 0x7FF;
}

static uint8_t pattern_pixel(int pat, int px, int py)
{
    uint32_t addr = ((uint32_t)(pat & 0x7FF) * 32 + py * 4 + (px >> 1)) & 0xFFFF;
    uint8_t byte = vdp.vram[addr];
    return (px & 1) ? (byte & 0x0F) : (byte >> 4);
}

static void get_window_bounds(int line, int screen_w, int *start, int *end)
{
    *start = 0;
    *end = 0;

    int wvp = vdp.regs[18] & 0x1F;
    int in_v;
    if (vdp.regs[18] & 0x80)
        in_v = (line >= wvp * 8);
    else
        in_v = (wvp > 0 && line < wvp * 8);
    if (!in_v) return;

    int whp = vdp.regs[17] & 0x1F;
    int whp_pix = whp * 16;
    if (vdp.regs[17] & 0x80) {
        if (whp_pix < screen_w) {
            *start = whp_pix;
            *end = screen_w;
        }
    } else {
        if (whp > 0) {
            *start = 0;
            *end = (whp_pix < screen_w) ? whp_pix : screen_w;
        }
    }
}

/*
 * Render one priority pass of a scrolling plane (A or B).
 * skip_start..skip_end defines a pixel range to skip (used to exclude the
 * window region when rendering Plane A).
 */
static void render_plane(uint32_t *row, int line, int plane_b, int pri,
                         int skip_start, int skip_end)
{
    int pw, ph;
    get_plane_size(&pw, &ph);
    int pw_pix = pw * 8;
    int ph_pix = ph * 8;

    uint16_t nt_base;
    if (plane_b)
        nt_base = (uint16_t)(vdp.regs[4] & 0x07) << 13;
    else
        nt_base = (uint16_t)(vdp.regs[2] & 0x38) << 10;

    int hs = get_hscroll(line, plane_b);
    int screen_w = (vdp.regs[12] & 0x81) ? 320 : 256;

    for (int x = 0; x < screen_w; x++) {
        if (x >= skip_start && x < skip_end)
            continue;

        int vs = get_vscroll(x, plane_b);
        int py = (line + vs) & (ph_pix - 1);
        int px = (x - hs) & (pw_pix - 1);

        int tc = px >> 3, tr = py >> 3;
        int fx = px & 7, fy = py & 7;

        uint32_t na = (nt_base + (tr * pw + tc) * 2) & 0xFFFF;
        uint16_t e = ((uint16_t)vdp.vram[na] << 8) | vdp.vram[(na + 1) & 0xFFFF];

        if (((e >> 15) & 1) != pri) continue;

        int pal = (e >> 13) & 3;
        int vf  = (e >> 12) & 1;
        int hf  = (e >> 11) & 1;
        int pat = e & 0x7FF;

        uint8_t pixel = pattern_pixel(pat, hf ? 7 - fx : fx, vf ? 7 - fy : fy);
        if (pixel == 0) continue;

        row[x] = cram_to_argb(vdp.cram[pal * 16 + pixel]);
    }
}

/*
 * Render one priority pass of the window plane.
 * The window has its own nametable (no scrolling) and only covers win_start..win_end.
 */
static void render_window(uint32_t *row, int line, int pri,
                          int win_start, int win_end)
{
    if (win_start >= win_end) return;

    int screen_w = (vdp.regs[12] & 0x81) ? 320 : 256;
    int nt_w = (screen_w == 320) ? 64 : 32;

    uint16_t nt_base;
    if (screen_w == 320)
        nt_base = (uint16_t)(vdp.regs[3] & 0x3C) << 10;
    else
        nt_base = (uint16_t)(vdp.regs[3] & 0x3E) << 10;

    int win_row = line >> 3;

    for (int x = win_start; x < win_end; x++) {
        int win_col = x >> 3;
        int fx = x & 7, fy = line & 7;

        uint32_t na = (nt_base + (win_row * nt_w + win_col) * 2) & 0xFFFF;
        uint16_t e = ((uint16_t)vdp.vram[na] << 8) | vdp.vram[(na + 1) & 0xFFFF];

        if (((e >> 15) & 1) != pri) continue;

        int pal = (e >> 13) & 3;
        int vf  = (e >> 12) & 1;
        int hf  = (e >> 11) & 1;
        int pat = e & 0x7FF;

        uint8_t pixel = pattern_pixel(pat, hf ? 7 - fx : fx, vf ? 7 - fy : fy);
        if (pixel == 0) continue;

        row[x] = cram_to_argb(vdp.cram[pal * 16 + pixel]);
    }
}

/* ------------------------------------------------------------------ */
/*  Scanline composition                                               */
/* ------------------------------------------------------------------ */

static void render_scanline(int line)
{
    uint32_t *row = &vdp.framebuffer[line * SCREEN_WIDTH];

    uint8_t bg_idx = vdp.regs[7] & 0x3F;
    uint32_t bg = cram_to_argb(vdp.cram[bg_idx]);
    for (int x = 0; x < SCREEN_WIDTH; x++)
        row[x] = bg;

    if (!(vdp.regs[1] & 0x40))
        return;

    int screen_w = (vdp.regs[12] & 0x81) ? 320 : 256;
    int win_start, win_end;
    get_window_bounds(line, screen_w, &win_start, &win_end);

    render_plane(row, line, 1, 0, 0, 0);
    render_plane(row, line, 0, 0, win_start, win_end);
    render_window(row, line, 0, win_start, win_end);

    render_plane(row, line, 1, 1, 0, 0);
    render_plane(row, line, 0, 1, win_start, win_end);
    render_window(row, line, 1, win_start, win_end);
}

void vdp_run_scanline(int line)
{
    vdp.line = line;
    vdp.hint_pending = 0;

    if (line == 0)
        vdp.hint_counter = vdp.regs[10];

    if (line < ACTIVE_LINES) {
        vdp.status &= ~ST_VBLANK;

        render_scanline(line);

        vdp.hint_counter--;
        if (vdp.hint_counter < 0) {
            vdp.hint_counter = vdp.regs[10];
            vdp.hint_pending = 1;
        }
    } else if (line == ACTIVE_LINES) {
        vdp.status |= ST_VBLANK | ST_VINT;
        vdp.hint_counter = vdp.regs[10];
    } else {
        vdp.hint_counter = vdp.regs[10];
    }

    vdp_update_ipl();
}

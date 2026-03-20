/*
 * Genesis I/O controller and system registers.
 *
 * Handles the version register, three controller/expansion ports with
 * TH-based multiplexing for 3-button pads, Z80 bus arbitration, and
 * the TMSS lock register.  Controller button state is hardcoded to
 * "all released" for now -- real input mapping comes in Phase 8.
 */

#include "io.h"
#include <string.h>

/* ------------------------------------------------------------------ */
/*  Version register                                                   */
/* ------------------------------------------------------------------ */

/*
 * Bit 7-6: region  (00=Japan domestic, 10=overseas)
 * Bit 5:   video   (1=NTSC, 0=PAL)
 * Bit 4:   expansion unit (0=not connected)
 * Bit 3-0: revision (0)
 *
 * 0xA0 = overseas NTSC, no expansion, rev 0  (US Genesis)
 */
#define VERSION_US_NTSC  0xA0

/* ------------------------------------------------------------------ */
/*  Controller port state                                              */
/* ------------------------------------------------------------------ */

typedef struct {
    uint8_t data;       /* Data register (directly written by 68K) */
    uint8_t ctrl;       /* Control register (1 = output, 0 = input) */
    uint8_t s_ctrl;     /* Serial control (stub) */
    uint8_t tx_data;    /* Serial TX (stub) */
    uint8_t rx_data;    /* Serial RX (stub) */
} io_port_t;

static struct {
    io_port_t port[3];          /* 0 = ctrl 1, 1 = ctrl 2, 2 = EXP */
    int       z80_bus_granted;
    int       z80_reset_active;
} io;

/* ------------------------------------------------------------------ */
/*  Init / Reset                                                       */
/* ------------------------------------------------------------------ */

void io_init(void)
{
    memset(&io, 0, sizeof(io));
    io.z80_bus_granted = 1;
    io.z80_reset_active = 1;
}

void io_reset(void)
{
    io_init();
}

/* ------------------------------------------------------------------ */
/*  Controller read (3-button pad, no buttons pressed)                 */
/* ------------------------------------------------------------------ */

/*
 * The 3-button Genesis controller multiplexes 8 buttons over 6 data
 * lines using the TH pin (active select, directly driven by bit 6):
 *
 *   TH=1  →  bit 5:C  4:B  3:Right  2:Left  1:Down  0:Up
 *   TH=0  →  bit 5:Start  4:A  3:0  2:0  1:Down  0:Up
 *
 * Active low: 1 = released, 0 = pressed.
 * With no buttons pressed:  TH=1 → 0x3F,  TH=0 → 0x33
 * Bits configured as output (ctrl=1) reflect the data register instead.
 */
static uint8_t controller_read(int idx)
{
    io_port_t *p = &io.port[idx];

    /* Determine TH level: if bit 6 of ctrl is output, use data reg;
     * otherwise TH floats high (pulled up). */
    uint8_t th = (p->ctrl & 0x40) ? ((p->data >> 6) & 1) : 1;

    uint8_t input;
    if (idx == 2) {
        input = 0x7F;       /* EXP port: no device */
    } else if (th) {
        input = 0x3F;       /* TH=1: all buttons released */
    } else {
        input = 0x33;       /* TH=0: all released, bits 3-2 grounded */
    }

    /* Output bits come from data register; input bits from controller */
    return (p->data & p->ctrl) | (input & ~p->ctrl);
}

/* ------------------------------------------------------------------ */
/*  Register read/write                                                */
/* ------------------------------------------------------------------ */

uint8_t io_read8(uint32_t addr)
{
    /* I/O chip registers: 0xA10000 - 0xA1001F
     * The I/O chip is 8-bit on D0-D7, so only odd addresses carry data.
     * Even address reads return 0x00 (open bus on D8-D15). */
    if (addr >= 0xA10000 && addr <= 0xA1001F) {
        if (!(addr & 1))
            return 0x00;

        switch (addr) {
        case 0xA10001: return VERSION_US_NTSC;
        case 0xA10003: return controller_read(0);
        case 0xA10005: return controller_read(1);
        case 0xA10007: return controller_read(2);
        case 0xA10009: return io.port[0].ctrl;
        case 0xA1000B: return io.port[1].ctrl;
        case 0xA1000D: return io.port[2].ctrl;
        case 0xA1000F: return io.port[0].tx_data;
        case 0xA10011: return io.port[0].rx_data;
        case 0xA10013: return io.port[0].s_ctrl;
        case 0xA10015: return io.port[1].tx_data;
        case 0xA10017: return io.port[1].rx_data;
        case 0xA10019: return io.port[1].s_ctrl;
        case 0xA1001B: return io.port[2].tx_data;
        case 0xA1001D: return io.port[2].rx_data;
        case 0xA1001F: return io.port[2].s_ctrl;
        default:       return 0xFF;
        }
    }

    /* Z80 bus request: 0xA11100 */
    if (addr == 0xA11100)
        return io.z80_bus_granted ? 0x01 : 0x00;
    if (addr == 0xA11101)
        return 0x00;

    /* Z80 reset: 0xA11200 - 0xA11201 */
    if (addr >= 0xA11200 && addr <= 0xA11201)
        return 0x00;

    /* TMSS: 0xA14000 - 0xA14003 (read returns 0) */
    if (addr >= 0xA14000 && addr <= 0xA14003)
        return 0x00;

    return 0xFF;
}

void io_write8(uint32_t addr, uint8_t val)
{
    /* I/O chip registers */
    if (addr >= 0xA10000 && addr <= 0xA1001F) {
        if (!(addr & 1))
            return;

        switch (addr) {
        case 0xA10003: io.port[0].data = val; break;
        case 0xA10005: io.port[1].data = val; break;
        case 0xA10007: io.port[2].data = val; break;
        case 0xA10009: io.port[0].ctrl = val; break;
        case 0xA1000B: io.port[1].ctrl = val; break;
        case 0xA1000D: io.port[2].ctrl = val; break;
        case 0xA1000F: io.port[0].tx_data = val; break;
        case 0xA10013: io.port[0].s_ctrl = val; break;
        case 0xA10015: io.port[1].tx_data = val; break;
        case 0xA10019: io.port[1].s_ctrl = val; break;
        case 0xA1001B: io.port[2].tx_data = val; break;
        case 0xA1001F: io.port[2].s_ctrl = val; break;
        }
        return;
    }

    /* Z80 bus request */
    if (addr == 0xA11100 || addr == 0xA11101) {
        io.z80_bus_granted = 1;     /* Always grant immediately */
        return;
    }

    /* Z80 reset */
    if (addr >= 0xA11200 && addr <= 0xA11201) {
        io.z80_reset_active = !(val & 0x01);
        return;
    }

    /* TMSS: accept and ignore */
    if (addr >= 0xA14000 && addr <= 0xA14003)
        return;
}

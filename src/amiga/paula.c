/*
 * Paula — Amiga 500 audio/disk/UART chip.
 * Phase 2: 4-channel audio register model + direct-mode tick.
 *
 * DMA fetch from chip RAM is deferred to Phase 3.
 */

#include "paula.h"
#include "cpu.h"
#include <string.h>
#include <stdio.h>

/* ------------------------------------------------------------------ */
/*  Lifecycle                                                           */
/* ------------------------------------------------------------------ */

void paula_init(paula_t *p)
{
    memset(p, 0, sizeof(*p));
}

void paula_reset(paula_t *p)
{
    memset(p, 0, sizeof(*p));
}

/* ------------------------------------------------------------------ */
/*  SET/CLR helper (shared by DMACON, INTENA, INTREQ, ADKCON)          */
/* ------------------------------------------------------------------ */

static void setclr(uint16_t *reg, uint16_t val)
{
    if (val & 0x8000)
        *reg |=  (val & 0x7FFF);
    else
        *reg &= ~(val & 0x7FFF);
}

/* ------------------------------------------------------------------ */
/*  Register write                                                      */
/* ------------------------------------------------------------------ */

void paula_write_reg(paula_t *p, uint16_t offset, uint16_t val)
{
    /* SET/CLR control registers.
     *
     * Field mapping — the IRQ logic (paula_update_irq / paula_assert_intreq)
     * uses p->intreq as the ENABLE register and p->adkcon as the REQUEST
     * register.  Route hardware writes accordingly:
     *   HW INTENA ($09A) → p->intreq  (enable mask for IRQ evaluation)
     *   HW INTREQ ($09C) → p->adkcon  (request bits for IRQ evaluation)
     *   HW ADKCON ($09E) → p->intena  (reused for audio/disk control)
     */
    switch (offset) {
    case 0x096: setclr(&p->dmacon, val); return;
    case 0x09A: /* INTENA */
        { static int iec = 0;
          if (iec < 20 && (val & 0x4000)) {
            iec++;
            fprintf(stderr, "[INTENA#%d] val=%04X %s master → intena=%04X\n",
                    iec, val, (val & 0x8000) ? "SET" : "CLR",
                    (val & 0x8000) ? (p->intreq | (val & 0x3FFF)) : (p->intreq & ~(val & 0x3FFF)));
          }
        }
        setclr(&p->intreq, val); paula_update_irq(p); return;
    case 0x09C: setclr(&p->adkcon, val); paula_update_irq(p); return;
    case 0x09E: setclr(&p->intena, val); return;
    default: break;
    }

    /* Per-channel registers: base 0x0A0, stride 0x10, six regs each */
    if (offset >= 0x0A0 && offset <= 0x0DA) {
        unsigned ch  = (offset - 0x0A0) / 0x10;
        unsigned reg = (offset - 0x0A0) % 0x10;
        paula_channel_t *c = &p->ch[ch];
        switch (reg) {
        case 0x0: /* AUDnLCH: high 3 bits of 24-bit DMA pointer */
            c->lc = ((uint32_t)(val & 0x0007) << 16) | (c->lc & 0x0000FFFF);
            break;
        case 0x2: /* AUDnLCL: low 16 bits */
            c->lc = (c->lc & 0x00070000) | val;
            break;
        case 0x4: /* AUDnLEN */
            c->len = val;
            break;
        case 0x6: /* AUDnPER */
            c->per = val;
            break;
        case 0x8: /* AUDnVOL: clamp to 64 */
            c->vol = (val > 64) ? 64 : (uint8_t)val;
            break;
        case 0xA: /* AUDnDAT: load sample from high byte, arm counter */
            c->sample  = (int8_t)(val >> 8);
            c->per_cnt = c->per;
            break;
        default: break;
        }
    }
}

/* ------------------------------------------------------------------ */
/*  Register read                                                       */
/* ------------------------------------------------------------------ */

uint16_t paula_read_reg(const paula_t *p, uint16_t offset)
{
    switch (offset) {
    case 0x002: return p->dmacon;
    case 0x010: return p->intena;   /* ADKCONR (field repurposed for ADKCON) */

    case 0x018: /* SERDATR — serial port status + data.
                   *
                   * Bit layout:
                   *   15: OVRUN   14: RBF   13: TBE   12: TSRE   11: RXD
                   *   10:9: (unused)   8:0: data (9-bit, keyboard uses low 8)
                   *
                   * Return 0x7FFD: RBF=1, TBE=1, TSRE=1, RXD=1, data=0xFD.
                   * Kickstart masks data with ANDI.B #$7F → keycode 0x7D.
                   *
                   * Two different ROM routines read SERDATR during boot:
                   *
                   *   FC30C0 (blink handler): reads data, checks for keycode
                   *     0x7F (init-done).  Keycode 0x7D != 0x7F, so its DBEQ
                   *     loops until D1 exhausts (6 retries), then BMI takes
                   *     the correct boot-continuation path at FC05F0.
                   *     Returning 0x7F would cause an infinite restart loop
                   *     because 0x7F triggers JMP FC237E (re-enter cold init).
                   *
                   *   FC223E (poll handler): checks RBF (bit 14), returns -1
                   *     if RBF=0.  RBF=1 here lets it read the keycode and
                   *     return immediately (positive D0 exits the caller loop).
                   *
                   * 0xFD simulates the Amiga keyboard self-test-OK code.
                   */
        return 0x7FFD;

    case 0x01C: return p->intreq;   /* INTENAR (p->intreq = enable mask) */
    case 0x01E: return p->adkcon;  /* INTREQR (p->adkcon = request bits) */
    default:    return 0;
    }
}

/* ------------------------------------------------------------------ */
/*  Interrupt controller                                                */
/* ------------------------------------------------------------------ */

/*
 * Field naming note (inherited from original implementation):
 *   p->intreq = hardware INTENA register (enable, bit 14 = master enable)
 *   p->adkcon = hardware INTREQ register (request)
 * This is backwards from what the names suggest.  The write offsets in
 * paula_write_reg() map 0x09C → intreq and 0x09E → adkcon, whereas
 * real hardware has INTENA at 0x09C and INTREQ at 0x09E.
 * All functions below are correct for this mapping.
 */

void paula_assert_intreq(paula_t *p, uint16_t bits)
{
    p->adkcon |= (bits & 0x7FFFu);
    paula_update_irq(p);
}

void paula_update_irq(paula_t *p)
{
    /* Master enable is bit 14 of hardware INTENA (= p->intreq field). */
    uint16_t active = 0;
    if (p->intreq & 0x4000u)
        active = p->intreq & p->adkcon & 0x3FFFu;

    int level = 0;
    if (active & 0x0007u) level = 1;   /* TBE / DSKBLK / SOFT  (bits 0-2) */
    if (active & 0x0008u) level = 2;   /* PORTS / CIA-A         (bit  3)   */
    if (active & 0x0070u) level = 3;   /* COPER / VERTB / BLIT  (bits 4-6) */
    if (active & 0x0780u) level = 4;   /* AUD0-3                (bits 7-10)*/
    if (active & 0x1800u) level = 5;   /* RBF / DSKSYN          (bits 11-12)*/
    if (active & 0x2000u) level = 6;   /* EXTER / CIA-B         (bit  13)  */

    cpu_ipl = level;
}

/* ------------------------------------------------------------------ */
/*  Tick                                                                */
/* ------------------------------------------------------------------ */

void paula_tick(paula_t *p, uint32_t color_clocks,
                int16_t *out_left, int16_t *out_right)
{
    for (uint32_t clk = 0; clk < color_clocks; clk++) {
        for (int i = 0; i < 4; i++) {
            paula_channel_t *c = &p->ch[i];
            if (c->per == 0 || c->per_cnt == 0)
                continue;
            if (--c->per_cnt == 0) {
                c->out_sample = (int16_t)((int16_t)c->sample * c->vol / 64);
                c->per_cnt    = c->per;
            }
        }
    }

    /* Stereo routing: left = ch0+ch3, right = ch1+ch2 */
    if (out_left)
        *out_left  = p->ch[0].out_sample + p->ch[3].out_sample;
    if (out_right)
        *out_right = p->ch[1].out_sample + p->ch[2].out_sample;
}

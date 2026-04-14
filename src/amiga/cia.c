/*
 * CIA 8520 — Amiga Complex Interface Adapter implementation.
 * See cia.h for full register documentation.
 */

#include "cia.h"
#include <string.h>

/* ------------------------------------------------------------------ */
/*  Lifecycle                                                           */
/* ------------------------------------------------------------------ */

void cia_init(cia_t *c)
{
    memset(c, 0, sizeof(*c));
    c->sdr = 0xFF;   /* idle serial line */
}

void cia_reset(cia_t *c)
{
    cia_init(c);
}

/* ------------------------------------------------------------------ */
/*  Internal: fire a timer underflow                                    */
/* ------------------------------------------------------------------ */

static void fire_underflow(cia_t *c, uint8_t icr_bit,
                           paula_t *paula, uint16_t intreq_bit)
{
    c->icr_data |= icr_bit;
    if (c->icr_mask & icr_bit) {
        c->icr_data |= CIA_ICR_IR;       /* IR: enabled interrupt pending */
        paula_assert_intreq(paula, intreq_bit);
    }
}

/* ------------------------------------------------------------------ */
/*  Register read                                                       */
/* ------------------------------------------------------------------ */

uint8_t cia_read(cia_t *c, uint8_t reg)
{
    switch (reg) {
    case CIA_PRA:
        /* Output bits from register; input pins float high. */
        return (c->pra & c->ddra) | (~c->ddra & 0xFFu);
    case CIA_PRB:
        return (c->prb & c->ddrb) | (~c->ddrb & 0xFFu);
    case CIA_DDRA:   return c->ddra;
    case CIA_DDRB:   return c->ddrb;
    case CIA_TALO:   return (uint8_t)(c->ta_cnt & 0xFFu);
    case CIA_TAHI:   return (uint8_t)(c->ta_cnt >> 8);
    case CIA_TBLO:   return (uint8_t)(c->tb_cnt & 0xFFu);
    case CIA_TBHI:   return (uint8_t)(c->tb_cnt >> 8);
    case CIA_TODLO:  return 0;
    case CIA_TODMID: return 0;
    case CIA_TODHI:  return 0;
    case CIA_SDR:    return c->sdr;
    case CIA_ICR: {
        /* Return pending status including IR flag; auto-clear. */
        uint8_t val = c->icr_data;
        c->icr_data = 0;
        return val;
    }
    case CIA_CRA:    return c->cra & (uint8_t)~CIA_CR_LOAD; /* LOAD always reads 0 */
    case CIA_CRB:    return c->crb & (uint8_t)~CIA_CR_LOAD;
    default:         return 0xFF;
    }
}

/* ------------------------------------------------------------------ */
/*  Register write                                                      */
/* ------------------------------------------------------------------ */

void cia_write(cia_t *c, uint8_t reg, uint8_t val)
{
    switch (reg) {
    case CIA_PRA:  c->pra  = val; break;
    case CIA_PRB:  c->prb  = val; break;
    case CIA_DDRA: c->ddra = val; break;
    case CIA_DDRB: c->ddrb = val; break;

    case CIA_TALO:
        c->ta_latch = (c->ta_latch & 0xFF00u) | val;
        break;
    case CIA_TAHI:
        c->ta_latch = ((uint16_t)val << 8) | (c->ta_latch & 0x00FFu);
        /* When timer is stopped, load latch into counter immediately. */
        if (!(c->cra & CIA_CR_START))
            c->ta_cnt = c->ta_latch;
        break;

    case CIA_TBLO:
        c->tb_latch = (c->tb_latch & 0xFF00u) | val;
        break;
    case CIA_TBHI:
        c->tb_latch = ((uint16_t)val << 8) | (c->tb_latch & 0x00FFu);
        if (!(c->crb & CIA_CR_START))
            c->tb_cnt = c->tb_latch;
        break;

    case CIA_SDR:
        c->sdr = val;
        break;

    case CIA_ICR:
        /* Bit 7 = SET(1) or CLR(0) for lower 7 bits of mask. */
        if (val & 0x80u)
            c->icr_mask |=  (val & 0x7Fu);
        else
            c->icr_mask &= ~(val & 0x7Fu);
        break;

    case CIA_CRA:
        /* LOAD bit is a strobe: immediately copy latch → counter. */
        if (val & CIA_CR_LOAD)
            c->ta_cnt = c->ta_latch;
        c->cra = val & (uint8_t)~CIA_CR_LOAD;
        break;

    case CIA_CRB:
        if (val & CIA_CR_LOAD)
            c->tb_cnt = c->tb_latch;
        c->crb = val & (uint8_t)~CIA_CR_LOAD;
        break;

    default: break;
    }
}

/* ------------------------------------------------------------------ */
/*  Tick                                                                */
/* ------------------------------------------------------------------ */

void cia_tick(cia_t *c, int eclocks, paula_t *paula, uint16_t intreq_bit)
{
    /* ---- TOD clock ------------------------------------------------- */
    /* Real TOD ticks at 50 Hz (PAL VSYNC).  E-clock ≈ 70938 Hz.
     * 70938 / 50 ≈ 1419 E-clocks per TOD tick. */
    c->tod_eclocks += eclocks;
    if (c->tod_eclocks >= 1419) {
        c->tod_eclocks -= 1419;
        c->tod_counter++;
    }

    /* ---- FLAG pin simulation --------------------------------------- */
    /* For CIA-B (INTREQ_EXTER), simulate a periodic FLAG edge.
     * On real hardware the FLAG pin is connected to the disk index/ready
     * signal.  Without a floppy disk, DSKRDY pulses periodically causing
     * FLAG interrupts.  This lets trackdisk.device detect "no disk" and
     * return from DoIO, allowing the strap module to display the hand. */
    if (c->flag_count > 0) {
        c->flag_count -= eclocks;
        if (c->flag_count <= 0) {
            c->flag_count = c->flag_period;
            c->icr_data |= CIA_ICR_FLG;
            if (c->icr_mask & CIA_ICR_FLG) {
                c->icr_data |= CIA_ICR_IR;
                paula_assert_intreq(paula, intreq_bit);
            }
        }
    }

    /* ---- Timer A -------------------------------------------------- */
    if ((c->cra & CIA_CR_START) && c->ta_latch > 0) {
        int rem = eclocks;
        while (rem > 0) {
            if ((int)c->ta_cnt <= rem) {
                rem -= (int)c->ta_cnt;
                fire_underflow(c, CIA_ICR_TA, paula, intreq_bit);
                if (c->cra & CIA_CR_RUNMODE) {
                    c->cra &= (uint8_t)~CIA_CR_START; /* one-shot: stop */
                    c->ta_cnt = c->ta_latch;
                    break;
                }
                c->ta_cnt = c->ta_latch;              /* continuous: reload */
            } else {
                c->ta_cnt -= (uint16_t)rem;
                rem = 0;
            }
        }
    }

    /* ---- Timer B -------------------------------------------------- */
    if ((c->crb & CIA_CR_START) && c->tb_latch > 0) {
        int rem = eclocks;
        while (rem > 0) {
            if ((int)c->tb_cnt <= rem) {
                rem -= (int)c->tb_cnt;
                fire_underflow(c, CIA_ICR_TB, paula, intreq_bit);
                if (c->crb & CIA_CR_RUNMODE) {
                    c->crb &= (uint8_t)~CIA_CR_START;
                    c->tb_cnt = c->tb_latch;
                    break;
                }
                c->tb_cnt = c->tb_latch;
            } else {
                c->tb_cnt -= (uint16_t)rem;
                rem = 0;
            }
        }
    }
}

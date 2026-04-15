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
    c->sdr = 0xFF;         /* idle serial line */
    c->pra_input = 0xFF;   /* all input pins float high by default */
    c->prb_input = 0xFF;
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
        /* Output bits from register; input bits from external pin state. */
        return (c->pra & c->ddra) | (c->pra_input & ~c->ddra);
    case CIA_PRB:
        return (c->prb & c->ddrb) | (c->prb_input & ~c->ddrb);
    case CIA_DDRA:   return c->ddra;
    case CIA_DDRB:   return c->ddrb;
    case CIA_TALO:   return (uint8_t)(c->ta_cnt & 0xFFu);
    case CIA_TAHI:   return (uint8_t)(c->ta_cnt >> 8);
    case CIA_TBLO:   return (uint8_t)(c->tb_cnt & 0xFFu);
    case CIA_TBHI:   return (uint8_t)(c->tb_cnt >> 8);
    case CIA_TODLO: {
        /* Reading low byte unlatches the counter. */
        uint32_t val = c->tod_latched ? c->tod_latch : c->tod_counter;
        c->tod_latched = 0;
        return (uint8_t)(val & 0xFFu);
    }
    case CIA_TODMID: {
        uint32_t val = c->tod_latched ? c->tod_latch : c->tod_counter;
        return (uint8_t)((val >> 8) & 0xFFu);
    }
    case CIA_TODHI: {
        /* Reading high byte latches all 3 bytes for atomic read. */
        c->tod_latch = c->tod_counter;
        c->tod_latched = 1;
        return (uint8_t)((c->tod_latch >> 16) & 0xFFu);
    }
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

    case CIA_TODLO:
        /* Writing low byte sets the counter (if CRB bit 7 = 0)
         * or sets the alarm (if CRB bit 7 = 1).  For now: set counter. */
        c->tod_counter = (c->tod_counter & 0xFFFF00u) | val;
        break;
    case CIA_TODMID:
        c->tod_counter = (c->tod_counter & 0xFF00FFu) | ((uint32_t)val << 8);
        break;
    case CIA_TODHI:
        c->tod_counter = (c->tod_counter & 0x00FFFFu) | ((uint32_t)val << 16);
        break;

    case CIA_SDR:
        c->sdr = val;
        break;

    case CIA_ICR:
        /* Bit 7 = SET(1) or CLR(0) for lower 7 bits of mask.
         * On real hardware, if a pending ICR data bit now matches a
         * newly enabled mask bit, the interrupt fires immediately.
         * We set a flag so the next cia_tick delivers it.  This is
         * critical for keyboard init: data arrives before the mask
         * is enabled, then keyboard.device enables the mask and the
         * pending data triggers the interrupt retroactively. */
        if (val & 0x80u)
            c->icr_mask |=  (val & 0x7Fu);
        else
            c->icr_mask &= ~(val & 0x7Fu);
        /* Immediate match: if any data bits now match the mask,
         * fire the interrupt via Paula right away. */
        if ((c->icr_data & c->icr_mask & 0x1Fu) && !(c->icr_data & CIA_ICR_IR)) {
            c->icr_data |= CIA_ICR_IR;
            if (c->paula)
                paula_assert_intreq(c->paula, c->intreq_bit);
        }
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

    /* Deferred ICR check disabled — immediate check in cia_write handles it */

    /* ---- Keyboard serial simulation (CIA-A only) ------------------- */
    /* Delivers power-up key stream ($FE init, $FD self-test OK) via
     * SDR + SP interrupt.  keyboard.device reads SDR on the SP interrupt
     * and recognises the self-test-OK code, completing its init.
     * Without this, keyboard.device hangs waiting for keyboard data,
     * blocking the entire InitResident chain. */
    if (c->kbd_queue_pos < c->kbd_queue_len) {
        c->kbd_countdown -= eclocks;
        if (c->kbd_countdown <= 0) {
            c->sdr = c->kbd_queue[c->kbd_queue_pos++];
            c->kbd_countdown = 7000;  /* ~100ms between bytes */
            /* Fire SP interrupt */
            c->icr_data |= CIA_ICR_SP;
            if (c->icr_mask & CIA_ICR_SP) {
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

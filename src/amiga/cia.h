/*
 * CIA 8520 — Amiga Complex Interface Adapter.
 *
 * CIA-A: odd bytes at 0xBFExxx  (accessed via amiga_cia_a in bus.c)
 * CIA-B: even bytes at 0xBFDxxx (accessed via amiga_cia_b in bus.c)
 *
 * Register index = bits [11:8] of the bus address:
 *   0  PRA   — Port A data
 *   1  PRB   — Port B data
 *   2  DDRA  — Port A data-direction (1 = output)
 *   3  DDRB  — Port B data-direction
 *   4  TALO  — Timer A low byte (latch write / counter read)
 *   5  TAHI  — Timer A high byte
 *   6  TBLO  — Timer B low byte
 *   7  TBHI  — Timer B high byte
 *   8  TODLO  — TOD low  (Chapter 2: always returns 0)
 *   9  TODMID — TOD mid  (Chapter 2: always returns 0)
 *  10  TODHI  — TOD high (Chapter 2: always returns 0)
 *  12  SDR   — Serial Data Register (Chapter 2: returns 0xFF)
 *  13  ICR   — Interrupt Control Register (SET/CLR mask on write, auto-clear on read)
 *  14  CRA   — Control Register A (timer A start/runmode/load)
 *  15  CRB   — Control Register B (timer B start/runmode/load)
 *
 * Timer behaviour (E-clock driven):
 *   The E-clock runs at CPU / 10, giving 45 ticks per PAL scanline.
 *   Each timer counts down from its latch value.  On underflow:
 *     - ICR status bit is set.
 *     - If the ICR mask enables that timer, the IR bit (bit 7) is also set
 *       and paula_assert_intreq() is called with the system's INTREQ bit.
 *     - Continuous mode (CRA/CRB bit 3 = 0): counter reloads from latch.
 *     - One-shot mode  (CRA/CRB bit 3 = 1): timer stops (START cleared).
 *
 * ICR write protocol (same SET/CLR convention as Paula):
 *   Bit 7 = 1 → OR lower 7 bits into mask.
 *   Bit 7 = 0 → AND NOT lower 7 bits into mask.
 *   Reading ICR returns pending status (including IR bit 7) and auto-clears.
 */

#ifndef AMIGA_CIA_H
#define AMIGA_CIA_H

#include <stdint.h>
#include "paula.h"

/* ---- CIA register index constants ----------------------------------- */
#define CIA_PRA    0u
#define CIA_PRB    1u
#define CIA_DDRA   2u
#define CIA_DDRB   3u
#define CIA_TALO   4u
#define CIA_TAHI   5u
#define CIA_TBLO   6u
#define CIA_TBHI   7u
#define CIA_TODLO  8u
#define CIA_TODMID 9u
#define CIA_TODHI  10u
#define CIA_SDR    12u
#define CIA_ICR    13u
#define CIA_CRA    14u
#define CIA_CRB    15u

/* ---- CRA / CRB bit masks -------------------------------------------- */
#define CIA_CR_START    0x01u   /* bit 0: timer start */
#define CIA_CR_RUNMODE  0x08u   /* bit 3: 0=continuous, 1=one-shot */
#define CIA_CR_LOAD     0x10u   /* bit 4: force latch → counter (strobe) */

/* ---- ICR bit masks --------------------------------------------------- */
#define CIA_ICR_TA  0x01u   /* Timer A underflow */
#define CIA_ICR_TB  0x02u   /* Timer B underflow */
#define CIA_ICR_IR  0x80u   /* IR: any enabled interrupt pending (read-only) */

/* ------------------------------------------------------------------ */
/*  Data type                                                           */
/* ------------------------------------------------------------------ */

typedef struct {
    uint8_t  pra, prb;      /* Port data registers */
    uint8_t  ddra, ddrb;    /* Port data-direction (1=output) */

    uint16_t ta_latch;      /* Timer A write latch */
    uint16_t ta_cnt;        /* Timer A running count */
    uint16_t tb_latch;      /* Timer B write latch */
    uint16_t tb_cnt;        /* Timer B running count */

    uint8_t  icr_mask;      /* Which ICR bits trigger an interrupt */
    uint8_t  icr_data;      /* Pending interrupt status (cleared on read) */

    uint8_t  sdr;           /* Serial Data Register */
    uint8_t  cra;           /* Control Register A */
    uint8_t  crb;           /* Control Register B */
} cia_t;

/* ------------------------------------------------------------------ */
/*  API                                                                 */
/* ------------------------------------------------------------------ */

/* Initialise all fields to 0; sdr preset to 0xFF (idle line). */
void cia_init(cia_t *c);

/* Reset (same as init for Chapter 2). */
void cia_reset(cia_t *c);

/*
 * Read register at index `reg` (0–15).
 * Port reads return: (pra & ddra) | (~ddra & 0xFF)  — output bits from
 * register, input bits float high (all 1s).
 * ICR read auto-clears icr_data.
 */
uint8_t cia_read(cia_t *c, uint8_t reg);

/*
 * Write `val` to register at index `reg`.
 * TAHI/TBHI writes also load the counter when the timer is stopped.
 * ICR write: bit 7 = SET(1)/CLR(0) for lower 7 bits of mask.
 * CRA/CRB LOAD bit (bit 4) is a strobe: loads latch into counter, not stored.
 */
void cia_write(cia_t *c, uint8_t reg, uint8_t val);

/*
 * Advance the CIA by `eclocks` E-clock ticks.
 * On timer underflow: sets ICR status bit; if that bit is in icr_mask,
 * also sets ICR_IR and calls paula_assert_intreq(paula, intreq_bit).
 *
 * intreq_bit: INTREQ_PORTS for CIA-A, INTREQ_EXTER for CIA-B.
 */
void cia_tick(cia_t *c, int eclocks, paula_t *paula, uint16_t intreq_bit);

#endif /* AMIGA_CIA_H */

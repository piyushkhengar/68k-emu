/*
 * Unit tests for the Amiga CIA 8520 chip (src/amiga/cia.c).
 *
 * Tests are written against the cia_t struct directly — no CPU execution,
 * no bus routing.  Each test creates its own cia_t and paula_t instances
 * so they are fully isolated.
 *
 * Coverage:
 *   - Init state (zero + sdr=0xFF)
 *   - Timer A/B latch write and read-back
 *   - Timer A countdown and underflow
 *   - Continuous vs one-shot modes
 *   - ICR mask SET/CLR write
 *   - ICR auto-clear on read
 *   - Timer underflow → ICR data → paula_assert_intreq → cpu_ipl chain
 *   - PRA OVL deactivation via bus (integration with bus.c globals)
 */

#include "cia.h"
#include "cia_tests.h"
#include "bus.h"     /* amiga_bus_init/read8/write8 for OVL test */
#include "cpu.h"     /* cpu_ipl */
#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/*  Test framework                                                      */
/* ------------------------------------------------------------------ */

static int failures;
static int total;

#define BASSERT(expr, fmt, ...) do {                                    \
    total++;                                                            \
    if (!(expr)) {                                                      \
        failures++;                                                     \
        printf("    FAIL [%s:%d]: " fmt "\n", __func__, __LINE__,      \
               ##__VA_ARGS__);                                          \
    }                                                                   \
} while (0)

/* ------------------------------------------------------------------ */
/*  Helpers                                                             */
/* ------------------------------------------------------------------ */

static cia_t   cia;
static paula_t paula;

static void reset_cia(void)
{
    cia_init(&cia);
    paula_init(&paula);
    cpu_ipl = 0;
}

/* ------------------------------------------------------------------ */
/*  Test: init state                                                    */
/* ------------------------------------------------------------------ */

static void test_cia_init_state(void)
{
    reset_cia();
    BASSERT(cia.pra == 0,        "pra should be 0 after init");
    BASSERT(cia.ddra == 0,       "ddra should be 0 after init");
    BASSERT(cia.ta_latch == 0,   "ta_latch should be 0 after init");
    BASSERT(cia.ta_cnt == 0,     "ta_cnt should be 0 after init");
    BASSERT(cia.icr_mask == 0,   "icr_mask should be 0 after init");
    BASSERT(cia.icr_data == 0,   "icr_data should be 0 after init");
    BASSERT(cia.sdr == 0xFF,     "sdr should be 0xFF after init");
    BASSERT(cia.cra == 0,        "cra should be 0 after init");
}

/* ------------------------------------------------------------------ */
/*  Test: port read with all-input direction returns 0xFF               */
/* ------------------------------------------------------------------ */

static void test_port_read_all_input(void)
{
    reset_cia();
    /* DDRA = 0 means all inputs; floating pins read as 1. */
    BASSERT(cia_read(&cia, CIA_PRA) == 0xFF,
            "PRA all-input: expected 0xFF, got 0x%02X",
            cia_read(&cia, CIA_PRA));
    BASSERT(cia_read(&cia, CIA_PRB) == 0xFF,
            "PRB all-input: expected 0xFF, got 0x%02X",
            cia_read(&cia, CIA_PRB));
}

/* ------------------------------------------------------------------ */
/*  Test: PRA output bits read back through direction mask              */
/* ------------------------------------------------------------------ */

static void test_port_read_output_bits(void)
{
    reset_cia();
    cia_write(&cia, CIA_DDRA, 0x0F);   /* low nibble = outputs */
    cia_write(&cia, CIA_PRA,  0x05);   /* set bits 0 and 2 */
    /* Output bits come from PRA; input bits (high nibble) float high. */
    uint8_t val = cia_read(&cia, CIA_PRA);
    BASSERT((val & 0x0F) == 0x05,
            "PRA output nibble: expected 0x05, got 0x%02X", val & 0x0F);
    BASSERT((val & 0xF0) == 0xF0,
            "PRA input nibble: expected 0xF0, got 0x%02X", val & 0xF0);
}

/* ------------------------------------------------------------------ */
/*  Test: timer A latch write and read-back                             */
/* ------------------------------------------------------------------ */

static void test_timer_a_latch_write(void)
{
    reset_cia();
    /* Write 0x1234 to timer A latch.
     * Timer not running → TAHI write also loads counter. */
    cia_write(&cia, CIA_TALO, 0x34);
    cia_write(&cia, CIA_TAHI, 0x12);
    BASSERT(cia.ta_latch == 0x1234,
            "ta_latch: expected 0x1234, got 0x%04X", cia.ta_latch);
    /* Counter loaded because timer stopped. */
    BASSERT(cia.ta_cnt == 0x1234,
            "ta_cnt loaded on stopped write: expected 0x1234, got 0x%04X",
            cia.ta_cnt);
    /* Read-back via registers. */
    BASSERT(cia_read(&cia, CIA_TALO) == 0x34,
            "TALO read-back: expected 0x34, got 0x%02X",
            cia_read(&cia, CIA_TALO));
    BASSERT(cia_read(&cia, CIA_TAHI) == 0x12,
            "TAHI read-back: expected 0x12, got 0x%02X",
            cia_read(&cia, CIA_TAHI));
}

/* ------------------------------------------------------------------ */
/*  Test: timer A basic countdown                                       */
/* ------------------------------------------------------------------ */

static void test_timer_a_countdown(void)
{
    reset_cia();
    cia_write(&cia, CIA_TALO, 0x0A);  /* latch = 10 */
    cia_write(&cia, CIA_TAHI, 0x00);
    cia_write(&cia, CIA_CRA, CIA_CR_START);  /* start, continuous */

    cia_tick(&cia, 5, &paula, INTREQ_PORTS);
    BASSERT(cia.ta_cnt == 5,
            "After 5 ticks of latch 10: expected cnt=5, got %u", cia.ta_cnt);
    BASSERT(cia.icr_data == 0,
            "No underflow yet: icr_data should be 0, got 0x%02X", cia.icr_data);
}

/* ------------------------------------------------------------------ */
/*  Test: timer A underflow sets ICR data bit                           */
/* ------------------------------------------------------------------ */

static void test_timer_a_underflow_icr(void)
{
    reset_cia();
    cia_write(&cia, CIA_TALO, 0x05);
    cia_write(&cia, CIA_TAHI, 0x00);
    cia_write(&cia, CIA_CRA, CIA_CR_START);

    cia_tick(&cia, 5, &paula, INTREQ_PORTS);

    BASSERT(cia.icr_data & CIA_ICR_TA,
            "After underflow: ICR_TA bit should be set, icr_data=0x%02X",
            cia.icr_data);
    /* Mask not enabled → no IR bit, no Paula assert. */
    BASSERT(!(cia.icr_data & CIA_ICR_IR),
            "Mask not set: ICR_IR should NOT be set, icr_data=0x%02X",
            cia.icr_data);
}

/* ------------------------------------------------------------------ */
/*  Test: timer A continuous mode reloads                               */
/* ------------------------------------------------------------------ */

static void test_timer_a_continuous_reload(void)
{
    reset_cia();
    cia_write(&cia, CIA_TALO, 0x03);
    cia_write(&cia, CIA_TAHI, 0x00);
    cia_write(&cia, CIA_CRA, CIA_CR_START);  /* continuous mode */

    /* Tick 6: two underflows (at t=3 and t=6). */
    cia_tick(&cia, 6, &paula, INTREQ_PORTS);

    /* After two underflows: counter reloaded twice.  At t=6 it just fired
     * and reloaded to 3, then consumed remaining 0 → cnt = 3. */
    BASSERT(cia.ta_cnt == 3,
            "Continuous reload: expected cnt=3, got %u", cia.ta_cnt);
    BASSERT(cia.cra & CIA_CR_START,
            "Continuous: timer must still be running");
}

/* ------------------------------------------------------------------ */
/*  Test: timer A one-shot mode stops after underflow                   */
/* ------------------------------------------------------------------ */

static void test_timer_a_one_shot(void)
{
    reset_cia();
    cia_write(&cia, CIA_TALO, 0x04);
    cia_write(&cia, CIA_TAHI, 0x00);
    cia_write(&cia, CIA_CRA, CIA_CR_START | CIA_CR_RUNMODE);  /* one-shot */

    cia_tick(&cia, 10, &paula, INTREQ_PORTS);

    BASSERT(!(cia.cra & CIA_CR_START),
            "One-shot: timer must have stopped, cra=0x%02X", cia.cra);
    BASSERT(cia.icr_data & CIA_ICR_TA,
            "One-shot: ICR_TA should be set");
}

/* ------------------------------------------------------------------ */
/*  Test: ICR mask SET/CLR write protocol                               */
/* ------------------------------------------------------------------ */

static void test_icr_mask_setclr(void)
{
    reset_cia();
    /* Set bits 0 and 1. */
    cia_write(&cia, CIA_ICR, 0x80 | 0x03);
    BASSERT(cia.icr_mask == 0x03,
            "ICR SET 0x03: expected mask=0x03, got 0x%02X", cia.icr_mask);

    /* Clear bit 0. */
    cia_write(&cia, CIA_ICR, 0x01);   /* bit7=0 → CLR */
    BASSERT(cia.icr_mask == 0x02,
            "ICR CLR 0x01: expected mask=0x02, got 0x%02X", cia.icr_mask);

    /* Set bit 2. */
    cia_write(&cia, CIA_ICR, 0x80 | 0x04);
    BASSERT(cia.icr_mask == 0x06,
            "ICR SET 0x04: expected mask=0x06, got 0x%02X", cia.icr_mask);
}

/* ------------------------------------------------------------------ */
/*  Test: ICR read auto-clears pending status                           */
/* ------------------------------------------------------------------ */

static void test_icr_auto_clear_on_read(void)
{
    reset_cia();
    /* Manually set icr_data to simulate a pending interrupt. */
    cia.icr_data = CIA_ICR_TA | CIA_ICR_IR;

    uint8_t val = cia_read(&cia, CIA_ICR);
    BASSERT(val == (CIA_ICR_TA | CIA_ICR_IR),
            "ICR read should return 0x%02X, got 0x%02X",
            (CIA_ICR_TA | CIA_ICR_IR), val);
    BASSERT(cia.icr_data == 0,
            "ICR auto-clear: icr_data must be 0 after read, got 0x%02X",
            cia.icr_data);

    /* Second read returns 0. */
    BASSERT(cia_read(&cia, CIA_ICR) == 0,
            "Second ICR read must return 0");
}

/* ------------------------------------------------------------------ */
/*  Test: timer underflow → ICR_IR + paula → cpu_ipl chain             */
/* ------------------------------------------------------------------ */

static void test_timer_irq_chain(void)
{
    reset_cia();

    /*
     * Enable ICR_TA in the mask so the underflow propagates to Paula.
     * Also enable Paula's INTENA master enable and PORTS (bit 3).
     *
     * Naming note: in this codebase's Paula struct,
     *   p->intreq holds hardware INTENA (enable register, bit 14 = master)
     *   p->adkcon holds hardware INTREQ (request register)
     * See paula.c for the full explanation.
     */
    cia_write(&cia, CIA_ICR, 0x80 | CIA_ICR_TA);  /* mask bit 0 = TA */

    /* Enable Paula master + PORTS in the enable register (p->intreq field). */
    paula_write_reg(&paula, 0x09C, 0x8000 | 0x4000 | INTREQ_PORTS);

    cia_write(&cia, CIA_TALO, 0x02);
    cia_write(&cia, CIA_TAHI, 0x00);
    cia_write(&cia, CIA_CRA, CIA_CR_START);

    cpu_ipl = 0;
    cia_tick(&cia, 2, &paula, INTREQ_PORTS);

    BASSERT(cia.icr_data & CIA_ICR_IR,
            "After enabled underflow: ICR_IR should be set, icr_data=0x%02X",
            cia.icr_data);
    BASSERT(cpu_ipl == 2,
            "CPU IPL should be 2 (PORTS level), got %d", cpu_ipl);
}

/* ------------------------------------------------------------------ */
/*  Test: LOAD strobe loads latch into counter                          */
/* ------------------------------------------------------------------ */

static void test_cra_load_strobe(void)
{
    reset_cia();
    cia_write(&cia, CIA_TALO, 0x20);
    cia_write(&cia, CIA_TAHI, 0x00);
    /* Start timer so TAHI write doesn't auto-load. */
    cia_write(&cia, CIA_CRA, CIA_CR_START);
    /* Count down a bit. */
    cia_tick(&cia, 5, &paula, INTREQ_PORTS);
    BASSERT(cia.ta_cnt == 0x1B, "pre-load cnt: expected 0x1B, got %u", cia.ta_cnt);

    /* LOAD strobe: reload latch into counter. */
    cia_write(&cia, CIA_CRA, CIA_CR_START | CIA_CR_LOAD);
    BASSERT(cia.ta_cnt == 0x20,
            "After LOAD: cnt should be latch=0x20, got %u", cia.ta_cnt);
    /* LOAD bit must not be stored in CRA. */
    BASSERT(!(cia.cra & CIA_CR_LOAD),
            "CRA LOAD bit must not persist, cra=0x%02X", cia.cra);
}

/* ------------------------------------------------------------------ */
/*  Test: OVL deactivation via CIA-A PRA through the bus               */
/* ------------------------------------------------------------------ */

static const uint8_t cia_test_rom[] = {
    0xDE, 0xAD, 0xBE, 0xEF,   /* bytes 0-3 */
    0x00, 0xFC, 0x00, 0x02,   /* bytes 4-7 */
    0xAA, 0xBB, 0xCC, 0xDD,
    0x11, 0x22, 0x33, 0x44,
};

static void test_cia_a_pra_clears_ovl_via_bus(void)
{
    amiga_bus_init(cia_test_rom, sizeof(cia_test_rom));

    /* OVL active: address 0 returns ROM byte 0 = 0xDE. */
    BASSERT(amiga_bus_read8(0x000000) == 0xDE,
            "pre-PRA: expected ROM 0xDE via OVL, got 0x%02X",
            amiga_bus_read8(0x000000));

    /* Kickstart sets DDRA bit 0 = output, then writes PRA bit 0 = 0.
     * Since PRA defaults to 0, the DDRA write drives pin LOW → OVL off. */
    amiga_bus_write8(0xBFE201, 0x03);  /* DDRA: bits 0,1 as output */
    amiga_bus_write8(0xBFE001, 0x02);  /* PRA: bit 0 = 0 → OVL off */

    /* OVL cleared: address 0 now exposes chip RAM (zeroed by init). */
    BASSERT(amiga_bus_read8(0x000000) == 0x00,
            "post-PRA: expected chip RAM 0x00, got 0x%02X",
            amiga_bus_read8(0x000000));
}

/* ------------------------------------------------------------------ */
/*  Test: cia_tick alone does NOT advance TOD                           */
/* ------------------------------------------------------------------ */
/* TOD pin is a discrete pulse input, not derived from E-clock.
 * cia_tick may run for many E-clocks without the TOD pin pulsing. */

static void test_cia_tick_does_not_advance_tod(void)
{
    reset_cia();
    for (int i = 0; i < 1000; i++) {
        cia_tick(&cia, 45, &paula, INTREQ_PORTS);
    }
    BASSERT(cia.tod_counter == 0,
            "cia_tick must not advance TOD, got %u", cia.tod_counter);
}

/* ------------------------------------------------------------------ */
/*  Test: cia_tod_tick increments by 1 (CIA-A VSYNC pattern)            */
/* ------------------------------------------------------------------ */
/* CIA-A's TOD pin is VSYNC: one pulse per PAL frame. */

static void test_tod_a_vsync_pattern(void)
{
    reset_cia();
    cia_tod_tick(&cia);
    BASSERT(cia.tod_counter == 1,
            "VSYNC tick should give TOD=1, got %u", cia.tod_counter);

    /* Three more frames worth of pulses. */
    cia_tod_tick(&cia);
    cia_tod_tick(&cia);
    cia_tod_tick(&cia);
    BASSERT(cia.tod_counter == 4,
            "Four VSYNC ticks should give TOD=4, got %u", cia.tod_counter);
}

/* ------------------------------------------------------------------ */
/*  Test: CIA-B HSYNC pattern — one tick per scanline                   */
/* ------------------------------------------------------------------ */
/* CIA-B's TOD pin is HSYNC: one pulse per scanline.  After one PAL
 * frame (312 lines), TOD should read 312. */

static void test_tod_b_hsync_pattern(void)
{
    reset_cia();
    for (int line = 0; line < 312; line++) {
        cia_tod_tick(&cia);
    }
    BASSERT(cia.tod_counter == 312,
            "312 HSYNC ticks should give TOD=312, got %u", cia.tod_counter);
}

/* ------------------------------------------------------------------ */
/*  Test: TOD wraps at 24 bits                                          */
/* ------------------------------------------------------------------ */

static void test_tod_24bit_wrap(void)
{
    reset_cia();
    cia.tod_counter = 0xFFFFFFu;
    cia_tod_tick(&cia);
    BASSERT(cia.tod_counter == 0,
            "TOD should wrap at 24 bits, got 0x%X", cia.tod_counter);
}

/* ------------------------------------------------------------------ */
/*  Test: timer B basic underflow                                       */
/* ------------------------------------------------------------------ */

static void test_timer_b_underflow(void)
{
    reset_cia();
    cia_write(&cia, CIA_TBLO, 0x03);
    cia_write(&cia, CIA_TBHI, 0x00);
    cia_write(&cia, CIA_CRB, CIA_CR_START);

    cia_tick(&cia, 3, &paula, INTREQ_PORTS);

    BASSERT(cia.icr_data & CIA_ICR_TB,
            "Timer B underflow: ICR_TB should be set, icr_data=0x%02X",
            cia.icr_data);
}

/* ------------------------------------------------------------------ */
/*  Entry point                                                         */
/* ------------------------------------------------------------------ */

int run_cia_tests(void)
{
    failures = 0;
    total    = 0;

    printf("CIA 8520 tests:\n");

    test_cia_init_state();
    test_port_read_all_input();
    test_port_read_output_bits();
    test_timer_a_latch_write();
    test_timer_a_countdown();
    test_timer_a_underflow_icr();
    test_timer_a_continuous_reload();
    test_timer_a_one_shot();
    test_icr_mask_setclr();
    test_icr_auto_clear_on_read();
    test_timer_irq_chain();
    test_cra_load_strobe();
    test_cia_a_pra_clears_ovl_via_bus();
    test_timer_b_underflow();
    test_cia_tick_does_not_advance_tod();
    test_tod_a_vsync_pattern();
    test_tod_b_hsync_pattern();
    test_tod_24bit_wrap();

    if (failures == 0)
        printf("  CIA 8520: all %d assertions passed.\n", total);
    else
        printf("  CIA 8520: %d/%d assertions FAILED.\n", failures, total);

    return failures;
}

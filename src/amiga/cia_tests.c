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
#include "bus.h"          /* amiga_bus_init/read8/write8 for OVL test */
#include "cpu.h"          /* cpu_ipl */
#include "disk_drive.h"   /* disk-presence integration tests */
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

    /* Enable Paula master + PORTS in the enable register (p->intreq field).
     * HW INTENA write is at $DFF09A, which paula_write_reg routes to
     * p->intreq (the field used as the enable mask in paula_update_irq). */
    paula_write_reg(&paula, 0x09A, 0x8000 | 0x4000 | INTREQ_PORTS);

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

/* ====================================================================
 *  Disk-drive presence (CIA-B PRB → disk_drive → CIA-A PRA inputs)
 * ====================================================================
 *
 * The strap module's "no disk in df0:" detection relies on accurate
 * disk-pin emulation: with no disk inserted, /CHNG must read low after a
 * step pulse, /RDY must stay high, /TK0 must report the head position.
 * These tests pin down that behaviour at the cia_t / disk_drive interface.
 */

/* Active-low CIA-B PRB output bits (mirrors disk_drive.c). */
#define PRB_MTR    0x80u
#define PRB_SEL0   0x08u
#define PRB_SEL1   0x10u
#define PRB_SIDE   0x04u
#define PRB_DIR    0x02u
#define PRB_STEP   0x01u

#define PRA_RDY    0x20u
#define PRA_TK0    0x10u
#define PRA_WPRO   0x08u
#define PRA_CHNG   0x04u

static void test_disk_drive_init_no_drive_selected(void)
{
    amiga_disk_drive_t d;
    amiga_disk_drive_init(&d);
    BASSERT(d.selected == -1, "init: selected should be -1, got %d", d.selected);
    BASSERT(!d.motor_on, "init: motor should be off");
    /* No drive selected → all four disk bits report inactive (high). */
    uint8_t s = amiga_disk_drive_pra_status(&d);
    BASSERT((s & PRA_RDY)  == PRA_RDY,  "init: /RDY should be high");
    BASSERT((s & PRA_TK0)  == PRA_TK0,  "init: /TK0 should be high");
    BASSERT((s & PRA_WPRO) == PRA_WPRO, "init: /WPRO should be high");
    BASSERT((s & PRA_CHNG) == PRA_CHNG, "init: /CHNG should be high");
}

static void test_disk_drive_select_df0_no_disk_chng_low(void)
{
    /* No disk in df0. Select df0 with motor on. */
    amiga_disk_drive_t d;
    amiga_disk_drive_init(&d);
    amiga_disk_drive_apply_prb(&d, (uint8_t)~(PRB_MTR | PRB_SEL0) & 0xFFu);
    BASSERT(d.selected == 0, "df0 selected: got %d", d.selected);
    BASSERT(d.motor_on,      "motor should be on after falling /SEL0 with /MTR=0");

    uint8_t s = amiga_disk_drive_pra_status(&d);
    /* /CHNG must be low (= bit clear) — drive sees no disk. */
    BASSERT((s & PRA_CHNG) == 0,
            "df0 no disk: /CHNG should be 0, got status=0x%02X", s);
    /* /RDY: motor on but no disk → stays HIGH (not ready). */
    BASSERT((s & PRA_RDY) == PRA_RDY,
            "df0 no disk: /RDY should stay high, got status=0x%02X", s);
    /* Head defaults to track 0 → /TK0 = 0. */
    BASSERT((s & PRA_TK0) == 0,
            "df0 head at track 0: /TK0 should be 0, got status=0x%02X", s);
    /* /WPRO: no disk → not write-protected (high). */
    BASSERT((s & PRA_WPRO) == PRA_WPRO,
            "df0 no disk: /WPRO should be high, got status=0x%02X", s);
}

/* Pulse /STEP low while a drive is selected. /STEP is active low; the falling
 * edge (high→low) is what real hardware steps on. */
static void pulse_step(amiga_disk_drive_t *d, uint8_t base_with_step_high)
{
    amiga_disk_drive_apply_prb(d, base_with_step_high);
    amiga_disk_drive_apply_prb(d, base_with_step_high & (uint8_t)~PRB_STEP);
    amiga_disk_drive_apply_prb(d, base_with_step_high);
}

static void test_disk_drive_step_no_disk_does_not_set_chng(void)
{
    amiga_disk_drive_t d;
    amiga_disk_drive_init(&d);
    /* Select df0 with motor on, /STEP idle (PRB_STEP bit set). */
    uint8_t base = (uint8_t)~(PRB_MTR | PRB_SEL0) & 0xFFu;
    amiga_disk_drive_apply_prb(&d, base);
    pulse_step(&d, base);

    uint8_t s = amiga_disk_drive_pra_status(&d);
    BASSERT((s & PRA_CHNG) == 0,
            "/STEP with no disk must NOT clear /CHNG flag, got 0x%02X", s);
}

static void test_disk_drive_step_with_disk_sets_chng(void)
{
    amiga_disk_drive_t d;
    amiga_disk_drive_init(&d);
    amiga_disk_drive_insert(&d, 0, false);
    uint8_t base = (uint8_t)~(PRB_MTR | PRB_SEL0) & 0xFFu;
    amiga_disk_drive_apply_prb(&d, base);
    /* Pre-step: /CHNG still low because the drive has not yet seen a step
     * since the disk was inserted (real hardware). */
    BASSERT((amiga_disk_drive_pra_status(&d) & PRA_CHNG) == 0,
            "pre-step with new disk: /CHNG should still be low");
    /* /STEP pulse falling edge while a disk is present: latch /CHNG high. */
    pulse_step(&d, base);
    BASSERT((amiga_disk_drive_pra_status(&d) & PRA_CHNG) == PRA_CHNG,
            "/STEP with disk: /CHNG should latch high");
    /* /RDY now low because motor is on AND disk present. */
    BASSERT((amiga_disk_drive_pra_status(&d) & PRA_RDY) == 0,
            "motor+disk: /RDY should be low (ready)");
}

static void test_disk_drive_step_advances_head(void)
{
    amiga_disk_drive_t d;
    amiga_disk_drive_init(&d);
    /* Select df0, motor on, /STEP idle, DIR=0 (inward → track++). */
    uint8_t base = (uint8_t)~(PRB_MTR | PRB_SEL0) & 0xFFu;
    base &= (uint8_t)~PRB_DIR;
    amiga_disk_drive_apply_prb(&d, base);
    for (int i = 0; i < 3; i++)
        pulse_step(&d, base);
    BASSERT(d.df[0].track == 3,
            "3 inward steps: track should be 3, got %d", d.df[0].track);
    BASSERT((amiga_disk_drive_pra_status(&d) & PRA_TK0) == PRA_TK0,
            "head off track 0: /TK0 should be high");

    /* Step outward (DIR=1 == bit set on PRB → track--). */
    base |= PRB_DIR;
    amiga_disk_drive_apply_prb(&d, base);
    pulse_step(&d, base);
    BASSERT(d.df[0].track == 2,
            "1 outward step from track 3: should be 2, got %d", d.df[0].track);
}

static void test_disk_drive_track_clamped_at_limits(void)
{
    amiga_disk_drive_t d;
    amiga_disk_drive_init(&d);
    uint8_t base = (uint8_t)~(PRB_MTR | PRB_SEL0) & 0xFFu;
    base |= PRB_DIR; /* DIR=1: outward */
    amiga_disk_drive_apply_prb(&d, base);
    pulse_step(&d, base);
    BASSERT(d.df[0].track == 0,
            "outward step at track 0: should clamp at 0, got %d", d.df[0].track);

    /* Step inward 100 times — must clamp at 79. */
    base &= (uint8_t)~PRB_DIR;
    amiga_disk_drive_apply_prb(&d, base);
    for (int i = 0; i < 100; i++)
        pulse_step(&d, base);
    BASSERT(d.df[0].track == 79,
            "100 inward steps: should clamp at 79, got %d", d.df[0].track);
}

static void test_disk_drive_eject_clears_chng_latch(void)
{
    amiga_disk_drive_t d;
    amiga_disk_drive_init(&d);
    amiga_disk_drive_insert(&d, 0, false);
    uint8_t base = (uint8_t)~(PRB_MTR | PRB_SEL0) & 0xFFu;
    amiga_disk_drive_apply_prb(&d, base);
    pulse_step(&d, base);
    BASSERT((amiga_disk_drive_pra_status(&d) & PRA_CHNG) == PRA_CHNG,
            "pre-eject /CHNG should be high after step");

    amiga_disk_drive_eject(&d, 0);
    /* /CHNG immediately low (drive notices ejection). */
    BASSERT((amiga_disk_drive_pra_status(&d) & PRA_CHNG) == 0,
            "post-eject /CHNG should be low");
    /* /RDY now high — disk gone. */
    BASSERT((amiga_disk_drive_pra_status(&d) & PRA_RDY) == PRA_RDY,
            "post-eject /RDY should go high again");
}

static void test_disk_drive_deselect_floats_pins_high(void)
{
    /* When /SELn is released, the bus pulls the lines high regardless of
     * what the (unselected) drive thinks. */
    amiga_disk_drive_t d;
    amiga_disk_drive_init(&d);
    amiga_disk_drive_insert(&d, 0, false);
    /* Select df0 + step + deselect. */
    uint8_t sel  = (uint8_t)~(PRB_MTR | PRB_SEL0) & 0xFFu;
    amiga_disk_drive_apply_prb(&d, sel);
    pulse_step(&d, sel);
    /* Deselect: all SELn high (and MTR irrelevant when nothing selected). */
    amiga_disk_drive_apply_prb(&d, 0xFFu);
    BASSERT(d.selected == -1, "no /SELn asserted: selected should be -1");
    uint8_t s = amiga_disk_drive_pra_status(&d);
    BASSERT(s == 0xFFu,
            "deselected drive should expose all-1s on PRA, got 0x%02X", s);
}

/* --- CIA-B PRB write hook fires post_write_hook ------------------- */

static int test_hook_count;
static uint8_t test_hook_last_reg;
static uint8_t test_hook_last_val;
static void test_hook_fn(cia_t *self, uint8_t reg, uint8_t val)
{
    (void)self;
    test_hook_count++;
    test_hook_last_reg = reg;
    test_hook_last_val = val;
}

static void test_cia_post_write_hook_fires_on_prb(void)
{
    reset_cia();
    test_hook_count = 0;
    cia.post_write_hook = test_hook_fn;
    cia_write(&cia, CIA_PRB, 0xF7);    /* /SEL0 asserted */
    BASSERT(test_hook_count == 1,
            "PRB write should fire post_write_hook once, got %d", test_hook_count);
    BASSERT(test_hook_last_reg == CIA_PRB,
            "hook reg: expected CIA_PRB, got %u", test_hook_last_reg);
    BASSERT(test_hook_last_val == 0xF7,
            "hook val: expected 0xF7, got 0x%02X", test_hook_last_val);
    cia.post_write_hook = NULL;
}

/* --- Integration: CIA-B PRB → disk_drive → CIA-A pra_input -------- */

static cia_t   integ_cia_a;
static cia_t   integ_cia_b;
static amiga_disk_drive_t integ_drive;

/* Same shape as amiga.c's hook: PRB writes update drive, then refresh
 * CIA-A's pra_input from drive status. CIA-A's non-disk PRA inputs (joystick
 * fire bits 7,6 + LED/OVL outputs 1,0) stay 1, matching the bus pull-ups. */
static void integ_cia_b_post_write(cia_t *self, uint8_t reg, uint8_t val)
{
    if (reg != CIA_PRB)
        return;
    (void)self;
    amiga_disk_drive_apply_prb(&integ_drive, val);
    integ_cia_a.pra_input = amiga_disk_drive_pra_status(&integ_drive);
}

static void test_cia_b_prb_write_updates_cia_a_pra_input(void)
{
    cia_init(&integ_cia_a);
    cia_init(&integ_cia_b);
    amiga_disk_drive_init(&integ_drive);
    integ_cia_a.pra_input = 0xFF;
    integ_cia_b.post_write_hook = integ_cia_b_post_write;

    /* DDRA = 0 so all PRA reads come from pra_input. */
    cia_write(&integ_cia_a, CIA_DDRA, 0);

    /* Initial state: nothing selected on CIA-B → CIA-A PRA reads 0xFF. */
    BASSERT(cia_read(&integ_cia_a, CIA_PRA) == 0xFF,
            "pre-select: CIA-A PRA should be 0xFF, got 0x%02X",
            cia_read(&integ_cia_a, CIA_PRA));

    /* Software writes CIA-B PRB to assert /MTR + /SEL0 (no disk in df0). */
    uint8_t prb = (uint8_t)~(PRB_MTR | PRB_SEL0) & 0xFFu;
    cia_write(&integ_cia_b, CIA_PRB, prb);

    uint8_t pra = cia_read(&integ_cia_a, CIA_PRA);
    BASSERT((pra & PRA_CHNG) == 0,
            "after /SEL0 with no disk: /CHNG must read 0 on CIA-A PRA, got 0x%02X",
            pra);
    BASSERT((pra & PRA_TK0) == 0,
            "head at track 0: /TK0 must read 0, got 0x%02X", pra);
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

    /* Disk-drive presence + PRB→PRA wiring. */
    test_disk_drive_init_no_drive_selected();
    test_disk_drive_select_df0_no_disk_chng_low();
    test_disk_drive_step_no_disk_does_not_set_chng();
    test_disk_drive_step_with_disk_sets_chng();
    test_disk_drive_step_advances_head();
    test_disk_drive_track_clamped_at_limits();
    test_disk_drive_eject_clears_chng_latch();
    test_disk_drive_deselect_floats_pins_high();
    test_cia_post_write_hook_fires_on_prb();
    test_cia_b_prb_write_updates_cia_a_pra_input();

    if (failures == 0)
        printf("  CIA 8520: all %d assertions passed.\n", total);
    else
        printf("  CIA 8520: %d/%d assertions FAILED.\n", failures, total);

    return failures;
}

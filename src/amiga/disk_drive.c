/*
 * Amiga floppy-drive presence model — see disk_drive.h for the full spec.
 *
 * This module owns the read-side of the disk pins. The write side (Paula's
 * DSKLEN/DSKPT/etc.) and MFM streaming are still ahead in Chapter 6.
 */

#include "disk_drive.h"
#include <string.h>

/* CIA-B PRB output bit positions (active LOW signals). */
#define PRB_MTR    0x80u
#define PRB_SEL3   0x40u
#define PRB_SEL2   0x20u
#define PRB_SEL1   0x10u
#define PRB_SEL0   0x08u
#define PRB_SIDE   0x04u
#define PRB_DIR    0x02u
#define PRB_STEP   0x01u

#define PRB_SEL_MASK (PRB_SEL3 | PRB_SEL2 | PRB_SEL1 | PRB_SEL0)

/* CIA-A PRA bit positions (active LOW disk inputs). */
#define PRA_RDY    0x20u   /* bit 5 */
#define PRA_TK0    0x10u   /* bit 4 */
#define PRA_WPRO   0x08u   /* bit 3 */
#define PRA_CHNG   0x04u   /* bit 2 */
#define PRA_DISK_BITS (PRA_RDY | PRA_TK0 | PRA_WPRO | PRA_CHNG)

/* ------------------------------------------------------------------ */
/*  Helpers                                                             */
/* ------------------------------------------------------------------ */

/*
 * Decode CIA-B PRB selection. /SELn lines are active low; the first asserted
 * line wins. Real hardware allows multi-select but driver code never does it,
 * so picking df0 first is good enough.
 */
static int decode_selected(uint8_t prb)
{
    if (!(prb & PRB_SEL0)) return 0;
    if (!(prb & PRB_SEL1)) return 1;
    if (!(prb & PRB_SEL2)) return 2;
    if (!(prb & PRB_SEL3)) return 3;
    return -1;
}

/* ------------------------------------------------------------------ */
/*  Lifecycle                                                           */
/* ------------------------------------------------------------------ */

void amiga_disk_drive_init(amiga_disk_drive_t *d)
{
    memset(d, 0, sizeof(*d));
    d->selected = -1;
    /* PRB defaults to all-1 at power up: drives deselected, motor off,
     * /STEP idle, DIR inward (don't-care). The first apply_prb that brings
     * a /SELn low therefore sees a real falling edge — exactly what real
     * hardware does, since drives latch /MTR on /SELn going low. */
    d->last_prb = 0xFF;
}

void amiga_disk_drive_reset(amiga_disk_drive_t *d)
{
    /* Preserve inserted disks; reset pin/control state. */
    bool        present[AMIGA_NUM_DRIVES];
    bool        wprot[AMIGA_NUM_DRIVES];
    for (int i = 0; i < AMIGA_NUM_DRIVES; i++) {
        present[i] = d->df[i].present;
        wprot[i]   = d->df[i].write_protected;
    }
    amiga_disk_drive_init(d);
    for (int i = 0; i < AMIGA_NUM_DRIVES; i++) {
        d->df[i].present         = present[i];
        d->df[i].write_protected = wprot[i];
        /* change_latch starts cleared after reset — software re-seats every
         * disk via TD_CHANGESTATE on boot. */
    }
}

void amiga_disk_drive_insert(amiga_disk_drive_t *d, int unit, bool write_protected)
{
    if (unit < 0 || unit >= AMIGA_NUM_DRIVES) return;
    d->df[unit].present         = true;
    d->df[unit].write_protected = write_protected;
    d->df[unit].change_latch    = false; /* drive sees fresh disk */
}

void amiga_disk_drive_eject(amiga_disk_drive_t *d, int unit)
{
    if (unit < 0 || unit >= AMIGA_NUM_DRIVES) return;
    d->df[unit].present      = false;
    d->df[unit].change_latch = false; /* /CHNG = 0 = "disk gone" */
}

/* ------------------------------------------------------------------ */
/*  PRB write — latch motor, decode select, process step               */
/* ------------------------------------------------------------------ */

void amiga_disk_drive_apply_prb(amiga_disk_drive_t *d, uint8_t prb)
{
    uint8_t old = d->last_prb;
    int     new_sel = decode_selected(prb);
    uint8_t falling = old & ~prb;            /* bits that just went 1→0 */

    /*
     * Real-hardware quirk: each drive latches the state of /MTR on the
     * falling edge of its OWN /SELn line. The latch persists after /SELn
     * is released, so the OS can spin up df0 while talking to df1 simply
     * by toggling /SEL0 with /MTR low.
     *
     * We approximate that with a single global motor_on flag — the OS
     * never spins multiple drives at once during early boot, and the
     * strap probe only ever talks to df0.
     */
    if (falling & PRB_SEL_MASK)
        d->motor_on = !(prb & PRB_MTR);

    /* /STEP falling edge: advance head while /SELn is asserted. */
    if ((falling & PRB_STEP) && new_sel >= 0) {
        amiga_drive_t *drv = &d->df[new_sel];
        int dir = (prb & PRB_DIR) ? -1 : +1;  /* DIR=0 → inward (track++) */
        int t = drv->track + dir;
        if (t < 0)  t = 0;
        if (t > 79) t = 79;
        drv->track = t;
        /*
         * On real hardware the drive sets the /CHNG output high after a
         * single step pulse if a disk is in the slot. With no disk, the
         * latch stays low forever. This is exactly what trackdisk's
         * TD_CHANGESTATE probe relies on: step → re-read /CHNG.
         */
        if (drv->present)
            drv->change_latch = true;
    }

    d->selected = new_sel;
    d->side     = (prb & PRB_SIDE) ? 0 : 1;
    d->last_prb = prb;
}

/* ------------------------------------------------------------------ */
/*  PRA status read                                                     */
/* ------------------------------------------------------------------ */

uint8_t amiga_disk_drive_pra_status(const amiga_disk_drive_t *d)
{
    /*
     * Default: no drive selected → bits 5..2 float high (pull-up to VCC).
     * Returning 0xFF lets caller AND this with the CIA pra_input mask.
     */
    uint8_t status = 0xFFu;

    if (d->selected < 0)
        return status;

    const amiga_drive_t *drv = &d->df[d->selected];

    /* /RDY: low when motor up AND disk present. */
    if (d->motor_on && drv->present)
        status &= (uint8_t)~PRA_RDY;

    /* /TK0: low when head at track 0 (regardless of disk). */
    if (drv->track == 0)
        status &= (uint8_t)~PRA_TK0;

    /* /WPRO: low when disk is write-protected (only meaningful with disk). */
    if (drv->present && drv->write_protected)
        status &= (uint8_t)~PRA_WPRO;

    /* /CHNG: low until the drive latches "disk OK" via insert + step. */
    if (!drv->change_latch)
        status &= (uint8_t)~PRA_CHNG;

    return status;
}

/*
 * Amiga floppy-drive presence model — the read-side of df0..df3.
 *
 * Real-hardware wiring:
 *   - CIA-B PRB drives the disk control outputs (active LOW):
 *       bit 7 = /MTR    (motor enable)
 *       bit 6 = /SEL3   (drive 3 select)
 *       bit 5 = /SEL2
 *       bit 4 = /SEL1
 *       bit 3 = /SEL0
 *       bit 2 = /SIDE   (head select)
 *       bit 1 = DIR     (step direction; 0 = inward / track++)
 *       bit 0 = /STEP   (step pulse on falling edge)
 *
 *   - The drive whose /SELn line is asserted drives four status lines back
 *     onto CIA-A PRA (active LOW):
 *       bit 5 = /RDY    (motor up to speed AND disk seated)
 *       bit 4 = /TK0    (head at track 0)
 *       bit 3 = /WPRO   (write protected)
 *       bit 2 = /CHNG   (cleared after eject; reasserted by step+disk-present)
 *
 * Scope here (Chapter 5 prerequisite, no MFM yet):
 *   - Track which drive is currently selected.
 *   - Latch /MTR on the falling edge of any /SELn (real hardware behavior).
 *   - Process /STEP on its falling edge: advance/retreat track, set the
 *     /CHNG latch high if a disk is present.
 *   - Expose `disk_drive_pra_status()` so CIA-A PRA reads return the four
 *     status bits for the currently selected drive (or all-1 when none is
 *     selected).
 *
 * Real MFM streaming and Paula disk DMA stay deferred to Chapter 6.
 */

#ifndef AMIGA_DISK_DRIVE_H
#define AMIGA_DISK_DRIVE_H

#include <stdbool.h>
#include <stdint.h>

#define AMIGA_NUM_DRIVES 4

typedef struct {
    bool present;            /* disk inserted in this drive */
    bool write_protected;    /* WPRO line low when disk inserted */
    int  track;              /* head cylinder, 0..79 */
    /*
     * /CHNG latch: cleared on eject (or boot with no disk), set when a step
     * pulse arrives while a disk is present. Without this, trackdisk would
     * accept the same disk repeatedly without re-reading the boot block.
     */
    bool change_latch;
} amiga_drive_t;

typedef struct {
    amiga_drive_t df[AMIGA_NUM_DRIVES];

    /* Currently selected drive: index 0..3, or -1 if /SEL3..0 are all high. */
    int     selected;
    bool    motor_on;        /* latched at falling edge of /SELn */
    int     side;            /* 0 = lower head, 1 = upper head (/SIDE inverted) */
    uint8_t last_prb;        /* previous PRB output, initialised to 0xFF (idle) */
} amiga_disk_drive_t;

/* ------------------------------------------------------------------ */
/*  API                                                                 */
/* ------------------------------------------------------------------ */

/* Power-up state: no disks, no drive selected, motor off, head at track 0. */
void amiga_disk_drive_init(amiga_disk_drive_t *d);

/* Reset to power-up state but keep inserted disks. */
void amiga_disk_drive_reset(amiga_disk_drive_t *d);

/* Insert a disk into df`unit`. Clears the /CHNG latch (drive notices change). */
void amiga_disk_drive_insert(amiga_disk_drive_t *d, int unit, bool write_protected);

/* Eject the disk in df`unit`. Clears the /CHNG latch. */
void amiga_disk_drive_eject(amiga_disk_drive_t *d, int unit);

/*
 * Apply a CIA-B PRB output value (the bits driven on the bus, NOT including
 * any input pull-ups). Updates selection, motor latch, head side, and steps
 * the head on the falling edge of /STEP.
 */
void amiga_disk_drive_apply_prb(amiga_disk_drive_t *d, uint8_t prb_value);

/*
 * Compute the byte that the currently selected drive presents on CIA-A PRA.
 *
 * Bits returned:
 *   bit 5 = /RDY    — 0 when motor on AND disk present
 *   bit 4 = /TK0    — 0 when head at track 0
 *   bit 3 = /WPRO   — 0 when disk is write protected
 *   bit 2 = /CHNG   — 0 until a step is seen while a disk is present
 *
 * All other bits (7,6,1,0) return 1, since this byte is OR'd with CIA-A's
 * non-disk PRA inputs (joystick fire buttons, /OVL/LED outputs).
 *
 * When no drive is selected, all four disk bits return 1 (deselected =
 * tri-state + pull-up to VCC).
 */
uint8_t amiga_disk_drive_pra_status(const amiga_disk_drive_t *d);

#endif /* AMIGA_DISK_DRIVE_H */

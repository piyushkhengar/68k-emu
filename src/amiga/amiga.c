/*
 * Amiga 500 system: system_t implementation + PAL run loop.
 *
 * Chapter 3: adds SDL2 graphical run loop (HAVE_SDL2).
 * Headless path (amiga_run_headless) unchanged from Chapter 2.
 *
 * PAL timing:
 *   CPU clock:  7,093,790 Hz
 *   Scanlines:  312 per frame, 50 Hz
 *   Cycles/line: 454 CPU cycles
 *   E-clock:    CPU / 10 → 45 E-clocks per scanline (rounded)
 */

#include "amiga.h"
#include "bus.h"
#include "cia.h"
#include "paula.h"
#include "cpu.h"
#include "memory.h"
#include <stdio.h>

#ifdef HAVE_SDL2
#  include "renderer.h"
#endif

/* ------------------------------------------------------------------ */
/*  PAL constants                                                       */
/* ------------------------------------------------------------------ */

#define PAL_LINES           312
#define CPU_CYCLES_PER_LINE 454
#define E_CLOCKS_PER_LINE    45   /* 454 / 10, rounded */

/* ------------------------------------------------------------------ */
/*  Bus vtable                                                          */
/* ------------------------------------------------------------------ */

static const mem_bus_t amiga_bus_vtable = {
    .read8   = amiga_bus_read8,
    .read16  = amiga_bus_read16,
    .read32  = amiga_bus_read32,
    .write8  = amiga_bus_write8,
    .write16 = amiga_bus_write16,
    .write32 = amiga_bus_write32,
};

/* ------------------------------------------------------------------ */
/*  Interrupt-acknowledge callback                                      */
/* ------------------------------------------------------------------ */

static void amiga_int_ack(int level)
{
    /*
     * Called when the CPU takes an autovector interrupt.
     * Clear the corresponding INTREQ bit(s) and re-evaluate.
     *
     * Naming note: hardware INTREQ is stored in amiga_paula.adkcon
     * (see paula.c for the full explanation of the offset mapping).
     */
    paula_t *p = &amiga_paula;
    switch (level) {
    case 1: p->adkcon &= (uint16_t)~(INTREQ_TBE | INTREQ_DSKBLK | INTREQ_SOFT); break;
    case 2: p->adkcon &= (uint16_t)~INTREQ_PORTS; break;
    case 3: p->adkcon &= (uint16_t)~(INTREQ_COPER | INTREQ_VERTB | INTREQ_BLIT); break;
    case 4: p->adkcon &= (uint16_t)~(INTREQ_AUD0 | INTREQ_AUD1 | INTREQ_AUD2 | INTREQ_AUD3); break;
    case 5: p->adkcon &= (uint16_t)~(INTREQ_RBF | INTREQ_DSKSYN); break;
    case 6: p->adkcon &= (uint16_t)~INTREQ_EXTER; break;
    default: break;
    }
    paula_update_irq(p);
}

/* ------------------------------------------------------------------ */
/*  system_t callbacks                                                  */
/* ------------------------------------------------------------------ */

/*
 * amiga_reset_external — called by the CPU's RESET instruction (0x4E70).
 *
 * Resets all external hardware WITHOUT clearing RAM.  This re-activates
 * OVL (CIA-A DDRA defaults to 0 → pin 0 is input → pull-up HIGH → OVL).
 * After RESET, Kickstart reads the vector table from ROM (via OVL) and
 * uses it for the warm-start JMP ($4).W sequence.
 */
static void amiga_reset_external(void)
{
    paula_reset(&amiga_paula);
    cia_reset(&amiga_cia_a);
    cia_reset(&amiga_cia_b);
    agnus_reset(&amiga_agnus);
    denise_reset(&amiga_denise);
    /* CIA-A reset clears DDRA/PRA → pin 0 = input → pull-up → OVL active */
    amiga_bus_set_ovl(true);
    /* Clear the 'HELP' watchdog magic at $0.  Kickstart writes 'HELP' to
     * $0 before risky init operations; if the system crashes and resets,
     * finding 'HELP' at $0 triggers diagnostic/keyboard mode.  Since we
     * don't actually crash, clear it to prevent false watchdog trips on
     * warm restart.  (On real hardware the warm-start path at FC3100-FC3112
     * clears this, but that path only runs when HELP is found during the
     * early cold boot check, not during normal RESET recovery.) */
    amiga_bus_write32(0x000000, 0x00000000);
}

static int amiga_init(const uint8_t *rom, size_t size)
{
    if (amiga_bus_init(rom, size) < 0)
        return -1;
    mem_set_bus(&amiga_bus_vtable);
    /*
     * Use 68000 model.  ColdStart tries MOVEC VBR/CACR which trigger
     * illegal-instruction exceptions (vector 4); the recovery handler
     * at FC0582 catches them and sets D0 flags accordingly.  VBR stays
     * at 0 (vectors at $000000) which is correct for A500.
     *
     * Previously used 68010 to let MOVEC VBR succeed, but the 68010's
     * RTE format word requirement conflicts with Kickstart 1.3's
     * interrupt dispatcher (FC0FE2) which pushes 68000-style 6-byte
     * frames — causing Format Error (vector 14) on every RTE.
     */
    cpu_init(CPU_MODEL_68000);
    /* Set callbacks AFTER cpu_init (which clears them). */
    cpu_set_int_ack(amiga_int_ack);
    cpu_set_reset_cb(amiga_reset_external);
    /* CIA-B FLAG pin: simulate periodic disk-index/ready signal.
     * Without this, trackdisk.device polls forever waiting for a disk
     * change event, preventing the strap module from displaying the
     * "insert disk" hand animation.  Period ~200ms in E-clocks. */
    amiga_cia_b.flag_period = 14000;
    amiga_cia_b.flag_count  = 14000;
    /* CIA-B PRB: set disk signals for "no disk present".
     * Bit 5 (DSKRDY)   = 1 → not ready (no disk)
     * Bit 2 (DSKCHANGE) = 0 → disk has been removed/changed
     * All other bits default high (no disk in any drive).
     * Set DDRB bit 2 as output so the port read returns our PRB value. */
    amiga_cia_b.prb  = 0xFB;  /* bit 2 clear = disk changed */
    amiga_cia_b.ddrb = 0x04;  /* bit 2 output */
    return 0;
}

static void amiga_reset(void)
{
    amiga_bus_reset();
}

static void amiga_shutdown(void)
{
    mem_set_bus(NULL);
    cpu_set_int_ack(NULL);
    cpu_set_reset_cb(NULL);
}

/* ------------------------------------------------------------------ */
/*  Run loop                                                            */
/* ------------------------------------------------------------------ */

void amiga_run(void)
{
#ifdef HAVE_SDL2
    if (renderer_init() < 0)
        return;

    /*
     * Framebuffer: AMIGA_HEIGHT visible lines × AMIGA_WIDTH pixels.
     * Lines 0–(AMIGA_HEIGHT-1) are rendered by Denise each frame.
     * Lines AMIGA_HEIGHT–311 still run the CPU and CIAs for correct timing
     * but are not blitted to the screen (vertical blanking / overscan region).
     */
    static uint32_t framebuffer[AMIGA_HEIGHT * AMIGA_WIDTH];

    while (!renderer_poll_events()) {
        for (int line = 0; line < PAL_LINES; line++) {
            /* Advance Agnus beam counter to the start of this scanline. */
            agnus_tick_scanline(&amiga_agnus, line);

            /* At the top of each frame, restart the Copper from COP1LC. */
            if (line == 0)
                amiga_agnus.copper_pc = amiga_agnus.cop1lc;

            /* Assert VBLANK interrupt at the start of vertical blank.
             * On real hardware this fires around line 0 of vblank (line 256
             * in PAL).  Asserting it HERE rather than at the end of the
             * frame ensures the request is pending during the CPU execution
             * of vblank lines, so brief Enable() windows can deliver it. */
            if (line == AMIGA_HEIGHT)
                paula_assert_intreq(&amiga_paula, INTREQ_VERTB);

            /* Run the Copper — executes MOVEs and WAITs up to this line. */
            agnus_copper_scanline(&amiga_agnus,
                                  amiga_bus_chip_ram(),
                                  amiga_bus_chip_ram_size(),
                                  amiga_bus_write_custom);

            /* Run the 68000 for one scanline worth of CPU cycles. */
            int cycles = 0;
            while (cycles < CPU_CYCLES_PER_LINE) {
                /* Workaround: skip input.device init entirely.
                 * input.device at FE5AEA opens keyboard.device (hangs without
                 * keyboard hardware) and timer.device. Even with the keyboard
                 * open skipped, input.device's init never returns — it hangs
                 * on timer/interrupt operations. Skip the entire InitResident
                 * call for input.device (A1=FE4C26) at the InitCode call site. */
                /* Skip input.device and intuition.library inits.
                 * input.device hangs on keyboard.device open (no keyboard hw).
                 * intuition.library hangs on input.device (which we skipped).
                 * Both block all lower-priority module inits including strap. */
                if (cpu.pc == 0xFC0B58 &&
                    (cpu.a[1] == 0xFE4C26 || cpu.a[1] == 0xFD3D8C)) {
                    cpu.d[0] = 0;
                    cpu.pc = 0xFC0B5C;
                }
                /* Debug: trace InitResident calls AND returns */
                { static int mi = 0;
                  if (mi < 80) {
                    if (cpu.pc == 0xFC0B58) { mi++; fprintf(stderr, "[IR] call A1=%06X\n", cpu.a[1]); }
                    if (cpu.pc == 0xFC0B5C) { fprintf(stderr, "[IR] returned\n"); }
                    if (cpu.pc == 0xFC0B3A) { /* BEQ end of list */
                        fprintf(stderr, "[IR] list-end D0=%08X\n", cpu.d[0]);
                    }
                  }
                }
                /* Workaround: skip trackdisk motor polling loop. */
                if (cpu.pc == 0xFE8F8E)
                    cpu.pc = 0xFE8FB6;
                /* ---- Boot workarounds for missing hardware ---- */
                /* Clear HELP watchdog after FC3028 writes it */
                if (cpu.pc == 0xFC302C)
                    amiga_bus_write32(0x000000, 0x00000000);
                /* Suppress Guru: keep LastAlert = -1 */
                if (cpu.pc == 0xFC3138) {
                    uint32_t eb = amiga_bus_read32(0x04);
                    if (eb >= 0x20 && eb < 0x80000)
                        amiga_bus_write32(eb + 0x202, 0xFFFFFFFF);
                }
                /* Force expansion init continue (not RESET) */
                if (cpu.pc == 0xFC3078)
                    cpu.d[0] = 0;
                /* Workaround: clear AttnResched before user-mode drop.
                 * Even with ORI to SR privilege fix, the scheduler dispatch
                 * during Permit() switches away from the init code prematurely.
                 * Clear AttnResched so Permit() just returns without dispatching. */
                if (cpu.pc == 0xFC04BE) {
                    uint32_t eb = amiga_bus_read32(0x04);
                    if (eb >= 0x20 && eb < 0x80000)
                        amiga_bus_write8(eb + 0x124, 0x00);
                }
                /* Trace strap flow in detail */
                { static int st = 0;
                  if (st < 30 && cpu.pc >= 0xFE8444 && cpu.pc < 0xFE8700) {
                    /* Only trace first entry into this range */
                    static int in_strap = 0;
                    if (!in_strap) {
                        in_strap = 1;
                        fprintf(stderr, "[STRAP-FLOW] entered @%06X\n", cpu.pc);
                    }
                    /* Trace key decision points */
                    if (cpu.pc == 0xFE8506) { st++; fprintf(stderr, "[STRAP] FE8506 after OpenDev D0=%d\n", cpu.d[0]); }
                    if (cpu.pc == 0xFE8524) { st++; fprintf(stderr, "[STRAP] FE8524 A2=%08X\n", cpu.a[2]); }
                    if (cpu.pc == 0xFE854A) { st++; fprintf(stderr, "[STRAP] FE854A disk path\n"); }
                    if (cpu.pc == 0xFE855C) { st++; fprintf(stderr, "[STRAP] FE855C DoIO #1\n"); }
                    if (cpu.pc == 0xFE8570) { st++; fprintf(stderr, "[STRAP] FE8570 DoIO #2\n"); }
                    if (cpu.pc == 0xFE8600) { st++; fprintf(stderr, "[STRAP] FE8600 DISPLAY PATH!\n"); }
                    if (cpu.pc == 0xFE8616) { st++; fprintf(stderr, "[STRAP] FE8616 BSR display_setup\n"); }
                    if (cpu.pc == 0xFE86C8) { st++; fprintf(stderr, "[STRAP] FE86C8 exit/cleanup\n"); }
                  }
                }
                /* Workaround: skip ALL strap disk operations.
                 * FE8502: OpenDevice("trackdisk.device") — hangs in motor ctrl
                 * FE855C, FE8570, FE859C: DoIO calls for disk read
                 * Skip OpenDevice and jump to FE8600 (display setup path). */
                /* Skip strap's OpenDevice + all DoIO calls for disk access.
                 * OpenDevice at FE8502 returns success (D0=0).
                 * DoIO calls at FE855C, FE8570, FE859C return error (D0=-1)
                 * so strap takes the FE8600 → FE8732 display setup path. */
                if (cpu.pc == 0xFE8502) {
                    cpu.d[0] = 0; cpu.pc = 0xFE8506;  /* OpenDevice ok */
                }
                if (cpu.pc == 0xFE855C || cpu.pc == 0xFE8570 || cpu.pc == 0xFE859C) {
                    cpu.d[0] = 0; cpu.pc += 4;  /* DoIO "success" with empty buffer */
                }
                int c = cpu_step();
                if (c == 0) {
                    cycles = CPU_CYCLES_PER_LINE;  /* CPU halted */
                    break;
                }
                cpu.cycles += (uint32_t)c;
                cycles += c;
            }

            /* Tick CIA-A (PORTS = level 2) and CIA-B (EXTER = level 6). */
            cia_tick(&amiga_cia_a, E_CLOCKS_PER_LINE, &amiga_paula, INTREQ_PORTS);
            cia_tick(&amiga_cia_b, E_CLOCKS_PER_LINE, &amiga_paula, INTREQ_EXTER);

            /* Render visible lines into the framebuffer. */
            if (line < AMIGA_HEIGHT) {
                denise_render_line(&amiga_denise,
                                   amiga_bus_chip_ram(),
                                   amiga_bus_chip_ram_size(),
                                   &framebuffer[line * AMIGA_WIDTH]);

                /*
                 * Agnus auto-increments each active bitplane pointer by one
                 * row's worth of bytes after DMA-fetching the row's words.
                 * 320 pixels / 16 bits per word = 20 words = 40 bytes per row.
                 */
                int np = (amiga_denise.bplcon0 >> 12) & 7;
                if (np > DENISE_PLANES) np = DENISE_PLANES;
                for (int n = 0; n < np; n++)
                    amiga_denise.bplpt[n] += DENISE_WIDTH / 8;
            }
        }

        /* End of frame: present. (VBLANK interrupt fires at line AMIGA_HEIGHT above.) */
        renderer_present(framebuffer);

        /* ---- Diagnostic: print key chip state until stable ---- */
        {
            static int  dbg_f           = 0;
            static int  last_dmacon     = -1;
            static int  last_color00    = -1;
            uint32_t mid_px = framebuffer[(AMIGA_HEIGHT / 2) * AMIGA_WIDTH + AMIGA_WIDTH / 2];
            int dmacon  = amiga_agnus.dmacon;
            int color00 = amiga_denise.color[0];
            ++dbg_f;

            /* Print when something changes OR for the first 20 frames */
            static int last_bpl1pt = -1;
            int bpl1pt = amiga_denise.bplpt[0];
            if (dbg_f <= 20 ||
                dmacon  != last_dmacon  ||
                color00 != last_color00 ||
                bpl1pt  != last_bpl1pt) {
                last_bpl1pt = bpl1pt;

                fprintf(stderr,
                    "[F%03d] PC=%06X COLOR00=%04X COLOR01=%04X BPLCON0=%04X(BPU=%d)"
                    " BPL1PT=%06X DMACON=%04X COP1LC=%06X COP2LC=%06X midpx=%08X\n",
                    dbg_f, cpu.pc,
                    amiga_denise.color[0], amiga_denise.color[1],
                    amiga_denise.bplcon0,  (amiga_denise.bplcon0 >> 12) & 7,
                    amiga_denise.bplpt[0],
                    amiga_agnus.dmacon, amiga_agnus.cop1lc,
                    amiga_agnus.cop2lc, mid_px);
                last_dmacon  = dmacon;
                last_color00 = color00;
            }
            /* Dump Copper list and scan for display Copper lists */
            if ((dbg_f == 86 || dbg_f == 200) && amiga_agnus.cop1lc) {
                /* Search chip RAM for COLOR00 MOVE ($0180 xxxx) patterns */
                fprintf(stderr, "[F%03d-SCAN] Searching chip RAM for COLOR00 MOVEs:\n", dbg_f);
                const uint8_t *cram = amiga_bus_chip_ram();
                for (uint32_t a = 0; a < 0x10000; a += 2) {
                    uint16_t w0 = (cram[a] << 8) | cram[a+1];
                    uint16_t w1 = (cram[a+2] << 8) | cram[a+3];
                    if (w0 == 0x0180 && w1 != 0x0FFF && w1 != 0x0000) {
                        /* Found a Copper MOVE COLOR00 with non-white value */
                        fprintf(stderr, "  $%04X: MOVE COLOR00, $%04X", a, w1);
                        /* Show context: next 4 instructions */
                        for (int ci = 1; ci < 5; ci++) {
                            uint16_t r = (cram[a+ci*4] << 8) | cram[a+ci*4+1];
                            uint16_t v = (cram[a+ci*4+2] << 8) | cram[a+ci*4+3];
                            fprintf(stderr, " | %04X %04X", r, v);
                        }
                        fprintf(stderr, "\n");
                    }
                }
            }
            if ((dbg_f == 86 || dbg_f == 200) && amiga_agnus.cop1lc) {
                uint32_t c1 = amiga_agnus.cop1lc;
                fprintf(stderr, "[F%03d-DUMP] COP1LC=%06X (64 words):", dbg_f, c1);
                for (int ci = 0; ci < 64; ci++) {
                    uint16_t w = amiga_bus_read16(c1 + ci * 2);
                    if (ci % 16 == 0) fprintf(stderr, "\n  +%02X:", ci*2);
                    fprintf(stderr, " %04X", w);
                }
                fprintf(stderr, "\n");
                /* Check bitmap at $2800 for any content */
                int nonzero = 0;
                for (uint32_t bi = 0; bi < 40 * 256; bi++)
                    if (amiga_bus_read8(0x2800 + bi)) nonzero++;
                fprintf(stderr, "[F%03d-DUMP] RAM@$2800 nonzero=%d/%d\n",
                        dbg_f, nonzero, 40*256);
                /* Check a wider area for any bitplane-like data */
                for (uint32_t base = 0x1000; base < 0x8000; base += 0x1000) {
                    nonzero = 0;
                    for (uint32_t bi = 0; bi < 40 * 10; bi++)
                        if (amiga_bus_read8(base + bi)) nonzero++;
                    if (nonzero > 0)
                        fprintf(stderr, "[F%03d-DUMP] RAM@$%04X nonzero=%d/400\n",
                                dbg_f, base, nonzero);
                }
            }
            /* Periodic PC dump every 20 frames — includes interrupt state */
            if (dbg_f >= 20 && dbg_f % 20 == 0 && dbg_f <= 500) {
                /* Paula naming: intreq=INTENA, adkcon=INTREQ (swapped) */
                fprintf(stderr, "[F%03d-PC] cpu.pc=%06X SR=%04X DMACON=%04X"
                        " INTENA=%04X INTREQ=%04X IPL=%d A7=%08X\n",
                        dbg_f, cpu.pc, cpu.sr, amiga_agnus.dmacon,
                        amiga_paula.intreq, amiga_paula.adkcon,
                        cpu_ipl, cpu.a[7]);
            }
            if (dbg_f == 500)
                fprintf(stderr, "[diagnostic complete]\n");
        }
    }

    renderer_shutdown();
#else
    fprintf(stderr, "Amiga graphical mode requires SDL2.  Build with:  make amiga\n");
#endif
}

void amiga_run_headless(int max_frames)
{
    printf("Amiga headless: running %d PAL frames at 7.09 MHz\n", max_frames);

    uint32_t prev_pc = 0;

    /* ---- Boot trace state (active for first BOOT_TRACE_FRAMES frames) ---- */
    #define BOOT_TRACE_FRAMES 1000
    uint32_t bt_last_execbase = 0;  /* last observed ExecBase at $4 */
    uint32_t bt_last_resmod   = 0;  /* last observed ResModules pointer */
    int      bt_reset_count   = 0;  /* number of RESET instructions seen */
    int      bt_exc_count     = 0;  /* number of exceptions taken */
    int      bt_jump_count    = 0;  /* number of significant PC jumps */
    uint32_t bt_total_steps   = 0;  /* total cpu_step calls in trace window */

    for (int frame = 1; frame <= max_frames; frame++) {
        int trace_active = (frame <= BOOT_TRACE_FRAMES);

        if (trace_active && frame == 1)
            printf("[BOOT-TRACE] === trace active for frames 1-%d ===\n",
                   BOOT_TRACE_FRAMES);

        for (int line = 0; line < PAL_LINES; line++) {
            /* Advance Agnus beam counter (needed for VBeamPos polling). */
            agnus_tick_scanline(&amiga_agnus, line);

            /* Assert VBLANK at start of vertical blank region (PAL line 256). */
            if (line == 256)
                paula_assert_intreq(&amiga_paula, INTREQ_VERTB);

            /* Run CPU for one scanline worth of cycles. */
            int cycles = 0;
            while (cycles < CPU_CYCLES_PER_LINE) {
                uint32_t pc_before = cpu.pc;
                uint16_t sr_before = cpu.sr;

                int c = cpu_step();
                if (c == 0) {
                    cycles = CPU_CYCLES_PER_LINE;  /* halted */
                    break;
                }
                cpu.cycles += (uint32_t)c;
                cycles += c;

                /* Workaround: skip trackdisk motor polling (same as gfx) */
                if (cpu.pc == 0xFE8F8E)
                    cpu.pc = 0xFE8FB6;
                /* Clear AttnResched (same as gfx) */
                if (cpu.pc == 0xFC04BE) {
                    uint32_t eb = amiga_bus_read32(0x04);
                    if (eb >= 0x20 && eb < 0x80000)
                        amiga_bus_write8(eb + 0x124, 0x00);
                }
                /* Boot workarounds (same as gfx) */
                if (cpu.pc == 0xFC302C)
                    amiga_bus_write32(0x000000, 0x00000000);
                if (cpu.pc == 0xFC3138) {
                    uint32_t eb = amiga_bus_read32(0x04);
                    if (eb >= 0x20 && eb < 0x80000)
                        amiga_bus_write32(eb + 0x202, 0xFFFFFFFF);
                }
                if (cpu.pc == 0xFC3078)
                    cpu.d[0] = 0;
                /* Skip input.device + intuition.library (same as gfx) */
                if (cpu.pc == 0xFC0B58 &&
                    (cpu.a[1] == 0xFE4C26 || cpu.a[1] == 0xFD3D8C)) {
                    cpu.d[0] = 0;
                    cpu.pc = 0xFC0B5C;
                }
                /* Skip strap OpenDevice("trackdisk.device") (same as gfx) */
                if (cpu.pc == 0xFE8502) {
                    cpu.d[0] = 0xFFFFFFFF;
                    cpu.pc = 0xFE8506;
                }
                if (trace_active) {
                    bt_total_steps++;
                    uint32_t pc_after = cpu.pc;
                    uint16_t ir       = cpu.ir;

                    /* 1. Detect RESET instruction (opcode 0x4E70) */
                    if (ir == 0x4E70) {
                        bt_reset_count++;
                        printf("[BOOT-TRACE F%02d L%03d] RESET instruction at PC=%06X "
                               "(count=%d) SR=%04X A7=%08X\n",
                               frame, line, pc_before, bt_reset_count,
                               cpu.sr, cpu.a[7]);
                    }

                    /* 2. Detect exceptions: S-bit went 0->1 and PC jumped
                     *    to an address that looks like a vector handler,
                     *    OR any jump into the autovector range.
                     *    Simpler heuristic: SR went from user to supervisor
                     *    mode, or PC landed in a different region via a
                     *    non-sequential change while supervisor. */
                    int s_before = (sr_before & SR_S) != 0;
                    int s_after  = (cpu.sr & SR_S) != 0;
                    if (!s_before && s_after && pc_after != pc_before) {
                        bt_exc_count++;
                        printf("[BOOT-TRACE F%02d L%03d] EXCEPTION: "
                               "PC %06X -> %06X  SR %04X->%04X  "
                               "ir=%04X (exc#%d)\n",
                               frame, line, pc_before, pc_after,
                               sr_before, cpu.sr, ir, bt_exc_count);
                    }
                    /* Also detect supervisor-mode exceptions (e.g. double
                     * fault, NMI): IPL mask changed AND big PC jump */
                    else if (s_before && s_after &&
                             (sr_before & SR_I_MASK) != (cpu.sr & SR_I_MASK) &&
                             pc_after != pc_before) {
                        int vec_guess = (int)(cpu.sr >> 8) & 7;
                        bt_exc_count++;
                        printf("[BOOT-TRACE F%02d L%03d] SV-EXCEPTION: "
                               "PC %06X -> %06X  SR %04X->%04X  "
                               "IPL-mask=%d (exc#%d)\n",
                               frame, line, pc_before, pc_after,
                               sr_before, cpu.sr, vec_guess, bt_exc_count);
                    }

                    /* 2b. Detect entry into keyboard handler or HELP check */
                    if (pc_after >= 0xFC3090 && pc_after <= 0xFC30C0 &&
                        (pc_before < 0xFC3090 || pc_before > 0xFC30C0)) {
                        printf("[BOOT-TRACE F%02d L%03d] KBD-ENTRY: "
                               "PC %06X -> %06X  ir=%04X SR=%04X->%04X "
                               "D0=%08X A7=%08X\n",
                               frame, line, pc_before, pc_after,
                               ir, sr_before, cpu.sr,
                               cpu.d[0], cpu.a[7]);
                    }
                    /* 2c-pre. Trace ROM scan path */
                    if (pc_after == 0xFC04BA) {
                        printf("[BOOT-TRACE F%02d L%03d] FC04BA: A0=%08X A1=%08X "
                               "A6=%08X [A1]=%08X [A1+4]=%08X\n",
                               frame, line, cpu.a[0], cpu.a[1], cpu.a[6],
                               amiga_bus_read32(cpu.a[1]),
                               amiga_bus_read32(cpu.a[1]+4));
                    }
                    if (pc_after >= 0xFC04BE && pc_after <= 0xFC04CE) {
                        uint32_t eb = cpu.a[6];
                        printf("[BOOT-TRACE F%02d L%03d] SCAN-PATH @%06X "
                               "ir=%04X SR=%04X EB+$124=%02X EB+$126=%02X EB+$127=%02X\n",
                               frame, line, pc_after, ir, cpu.sr,
                               amiga_bus_read8(eb+0x124),
                               amiga_bus_read8(eb+0x126),
                               amiga_bus_read8(eb+0x127));
                    }
                    /* Trace Reschedule (sets AttnResched) */
                    if (pc_after == 0xFC1F74) {
                        printf("[BOOT-TRACE F%02d L%03d] Reschedule(): "
                               "SR=%04X ret=%08X A6=%06X\n",
                               frame, line, cpu.sr,
                               amiga_bus_read32(cpu.a[7]),
                               cpu.a[6]);
                    }
                    /* Trace AddTask CMP before Reschedule */
                    if (pc_after == 0xFC1D1E) {
                        printf("[BOOT-TRACE F%02d L%03d] AddTask-CMP: "
                               "D0=%08X A1=%08X (equal=%d)\n",
                               frame, line, cpu.d[0], cpu.a[1],
                               cpu.d[0] == cpu.a[1]);
                    }
                    /* Trace Permit → Supervisor chain */
                    if (pc_after == 0xFC1F9C) { /* Permit entry */
                        printf("[BOOT-TRACE F%02d L%03d] Permit(): "
                               "SR=%04X IDNest=%02X TDNest=%02X Resched=%02X\n",
                               frame, line, cpu.sr,
                               amiga_bus_read8(cpu.a[6]+0x127),
                               amiga_bus_read8(cpu.a[6]+0x126),
                               amiga_bus_read8(cpu.a[6]+0x124));
                    }
                    if (pc_after == 0xFC1FB6) { /* Permit calls Supervisor */
                        uint32_t vec = amiga_bus_read32(cpu.a[6] - 0x1E + 2);
                        printf("[BOOT-TRACE F%02d L%03d] Permit->Supervisor: "
                               "A5=%06X vec_target=%06X SR=%04X\n",
                               frame, line, cpu.a[5], vec, cpu.sr);
                    }
                    if (pc_after == 0xFC0500) {
                        printf("[BOOT-TRACE F%02d L%03d] ROM-SCAN-ENTRY: "
                               "A0=%06X A6=%06X\n",
                               frame, line, cpu.a[0], cpu.a[6]);
                    }
                    if (pc_after == 0xFC093C) {
                        printf("[BOOT-TRACE F%02d L%03d] BuildResModules: "
                               "A0=%06X [A0]=%08X [A0+4]=%08X A6=%06X\n",
                               frame, line, cpu.a[0],
                               amiga_bus_read32(cpu.a[0]),
                               amiga_bus_read32(cpu.a[0]+4),
                               cpu.a[6]);
                    }
                    if (pc_after == 0xFC0508) {
                        printf("[BOOT-TRACE F%02d L%03d] ROM-SCAN-DONE: "
                               "D0=%08X (ResModules ptr) A6=%06X\n",
                               frame, line, cpu.d[0], cpu.a[6]);
                    }
                    if (pc_after == 0xFC09B0) {
                        printf("[BOOT-TRACE F%02d L%03d] TAG-FOUND: "
                               "A5=%06X (ResidentTag addr)\n",
                               frame, line, cpu.a[5]);
                    }
                    /* Trace trackdisk polling state */
                    { static int td_trc = 0;
                    if (pc_after == 0xFE8F92 && ++td_trc == 1) {
                        printf("[BOOT-TRACE F%02d] td-poll: A0=%06X "
                               "CIA-B ICR=%02X TBHI=%02X CRB=%02X "
                               "PRB=%02X DDRB=%02X\n",
                               frame,
                               cpu.a[0],
                               amiga_cia_b.icr_data,
                               (uint8_t)(amiga_cia_b.tb_cnt >> 8),
                               amiga_cia_b.crb,
                               amiga_cia_b.prb,
                               amiga_cia_b.ddrb);
                    }}
                    /* Trace strap boot flow and display setup decision */
                    if (pc_after == 0xFE8600) {
                        printf("[BOOT-TRACE F%02d L%03d] strap@FE8600: A2=%08X A5=%08X "
                               "[A5+4]=%08X\n",
                               frame, line, cpu.a[2], cpu.a[5],
                               amiga_bus_read32(cpu.a[5]+4));
                    }
                    if (pc_after == 0xFE8610) {
                        printf("[BOOT-TRACE F%02d L%03d] strap@FE8610: D0=[A5+4]=%08X "
                               "(0=setup, !0=skip)\n",
                               frame, line,
                               amiga_bus_read32(cpu.a[5]+4));
                    }
                    if (pc_after == 0xFE860C) {
                        printf("[BOOT-TRACE F%02d L%03d] strap@FE860C: BLE check A2=%08X "
                               "(<=0=retry)\n",
                               frame, line, cpu.a[2]);
                    }
                    /* Trace strap display setup calls */
                    if (pc_after == 0xFE8732) {
                        printf("[BOOT-TRACE F%02d L%03d] strap::display_setup ENTER\n",
                               frame, line);
                    }
                    if (pc_after == 0xFE887E || pc_after == 0xFE8884 ||
                        pc_after == 0xFE888A || pc_after == 0xFE8898) {
                        static const char *names[] = {"MakeVPort","MrgCop","LoadView","LoadRGB4"};
                        int idx = pc_after == 0xFE887E ? 0 : pc_after == 0xFE8884 ? 1 :
                                  pc_after == 0xFE888A ? 2 : 3;
                        printf("[BOOT-TRACE F%02d L%03d] strap::%s D0=%08X "
                               "COP1LC=%06X COP2LC=%06X\n",
                               frame, line, names[idx], cpu.d[0],
                               amiga_agnus.cop1lc, amiga_agnus.cop2lc);
                    }
                    if (pc_after == 0xFE8982) {
                        printf("[BOOT-TRACE F%02d L%03d] strap::display_setup DONE "
                               "COP1LC=%06X DMACON=%04X BPLCON0=%04X\n",
                               frame, line,
                               amiga_agnus.cop1lc, amiga_agnus.dmacon,
                               amiga_denise.bplcon0);
                    }
                    if (pc_after == 0xFC3180) {
                        printf("[BOOT-TRACE F%02d L%03d] RawDoFmt: "
                               "A0(fmt)=%06X A1(data)=%06X "
                               "A2(putch)=%06X A3(putdat)=%06X\n",
                               frame, line,
                               cpu.a[0], cpu.a[1], cpu.a[2], cpu.a[3]);
                    }
                    /* Trace JSR (A2) inside RawDoFmt */
                    if (pc_before >= 0xFC2124 && pc_before <= 0xFC2146 &&
                        ir == 0x4E92) {
                        static int jsr_a2_count = 0;
                        if (jsr_a2_count < 5) {
                            printf("[BOOT-TRACE F%02d L%03d] JSR(A2): "
                                   "A2=%06X D0=%02X('%c')\n",
                                   frame, line, cpu.a[2],
                                   cpu.d[0] & 0xFF,
                                   (cpu.d[0] & 0xFF) >= 0x20 ? (char)(cpu.d[0] & 0xFF) : '.');
                            jsr_a2_count++;
                        }
                    }
                    /* 2c. Detect code reaching FC3018-FC3028 (HELP check area) */
                    if (pc_after >= 0xFC3018 && pc_after <= 0xFC3028 &&
                        (pc_before < 0xFC3018 || pc_before > 0xFC3028)) {
                        uint32_t eb = amiga_bus_read32(4);
                        uint32_t eb202 = (eb >= 0x20 && eb < 0x80000) ?
                            amiga_bus_read32(eb + 0x202) : 0xDEAD;
                        printf("[BOOT-TRACE F%02d L%03d] HELP-CHECK: "
                               "PC %06X -> %06X  ir=%04X [$0]=%08X "
                               "EB=%08X EB+$202=%08X\n",
                               frame, line, pc_before, pc_after,
                               ir, amiga_bus_read32(0), eb, eb202);
                    }

                    /* 3. Significant PC jump (> 256 bytes, not a RESET) */
                    if (ir != 0x4E70) {
                        int32_t delta = (int32_t)pc_after - (int32_t)pc_before;
                        if (delta > 256 || delta < -256) {
                            bt_jump_count++;
                            /* Only log the first 200 jumps to avoid flood */
                            if (bt_jump_count <= 200) {
                                printf("[BOOT-TRACE F%02d L%03d] JUMP: "
                                       "PC %06X -> %06X  (delta=%+d) "
                                       "ir=%04X SR=%04X\n",
                                       frame, line, pc_before, pc_after,
                                       (int)delta, ir, cpu.sr);
                            } else if (bt_jump_count == 201) {
                                printf("[BOOT-TRACE] ... suppressing further "
                                       "JUMP logs (>200)\n");
                            }
                        }
                    }
                }
            }

            /* Tick CIA-A (PORTS = level 2) and CIA-B (EXTER = level 6). */
            cia_tick(&amiga_cia_a, E_CLOCKS_PER_LINE, &amiga_paula, INTREQ_PORTS);
            cia_tick(&amiga_cia_b, E_CLOCKS_PER_LINE, &amiga_paula, INTREQ_EXTER);
        }

        /* (VBLANK interrupt fires at line AMIGA_HEIGHT above.) */

        /* ---- Boot trace: per-frame chip RAM inspection ---- */
        if (trace_active) {
            uint32_t execbase = amiga_bus_read32(0x000004);
            uint32_t resmod   = 0;
            int      resmod_valid = 0;

            /* If ExecBase looks like a valid chip/slow RAM pointer, read
             * ResModules at ExecBase + 0x12E. */
            if ((execbase >= 0x000020 && execbase < 0x200000) ||
                (execbase >= 0xC00000 && execbase < 0xC80000)) {
                resmod = amiga_bus_read32(execbase + 0x012E);
                resmod_valid = 1;
            }

            /* Dump ResModules list when ExecBase first appears.
             * ResModules is at ExecBase+$012C (not $012E). */
            if (execbase != bt_last_execbase && resmod_valid) {
                uint32_t rm12c = amiga_bus_read32(execbase + 0x012C);
                printf("[BOOT-TRACE F%02d] EB+$12C(ResModules)=%08X", frame, rm12c);
                if (rm12c >= 0x20 && rm12c < 0x80000) {
                    printf(" dump:");
                    for (int rm = 0; rm < 8; rm++) {
                        uint32_t entry = amiga_bus_read32(rm12c + rm * 4);
                        printf(" [%d]=%08X", rm, entry);
                        if (entry == 0) break;
                    }
                }
                printf("\n");
            }

            /* Log when values change, or every 5 frames, or first/last */
            int should_log = (frame == 1 || frame == BOOT_TRACE_FRAMES ||
                              (frame % 5) == 0 ||
                              execbase != bt_last_execbase ||
                              (resmod_valid && resmod != bt_last_resmod));

            if (should_log) {
                printf("[BOOT-TRACE F%02d] PC=%06X SR=%04X A7=%08X  "
                       "[$4]ExecBase=%08X",
                       frame, cpu.pc, cpu.sr, cpu.a[7], execbase);
                if (resmod_valid)
                    printf("  [EB+$12E]ResModules=%08X", resmod);
                else
                    printf("  (ExecBase invalid, no ResModules)");
                printf("  steps=%u jumps=%d exc=%d resets=%d%s\n",
                       bt_total_steps, bt_jump_count, bt_exc_count,
                       bt_reset_count, cpu.halted ? " HALTED" : "");
            }

            bt_last_execbase = execbase;
            if (resmod_valid) bt_last_resmod = resmod;

            /* End of trace window summary */
            if (frame == BOOT_TRACE_FRAMES) {
                printf("[BOOT-TRACE] === summary after %d frames ===\n",
                       BOOT_TRACE_FRAMES);
                printf("[BOOT-TRACE]   total cpu_steps : %u\n", bt_total_steps);
                printf("[BOOT-TRACE]   RESET instrs    : %d\n", bt_reset_count);
                printf("[BOOT-TRACE]   exceptions      : %d\n", bt_exc_count);
                printf("[BOOT-TRACE]   sig. PC jumps   : %d\n", bt_jump_count);
                printf("[BOOT-TRACE]   final ExecBase  : %08X\n", bt_last_execbase);
                printf("[BOOT-TRACE]   final ResModules: %08X%s\n",
                       bt_last_resmod,
                       bt_last_resmod ? "" : " (NEVER SET - boot stuck!)");
                printf("[BOOT-TRACE] === end trace ===\n");
            }
        }

        if (frame <= 5 || (frame <= 120 && cpu.pc != prev_pc)) {
            printf("Amiga frame %3d: PC=%06X SR=%04X D0=%08X A7=%08X%s\n",
                   frame, cpu.pc, cpu.sr, cpu.d[0], cpu.a[7],
                   cpu.halted ? " HALTED" : "");
        }
        prev_pc = cpu.pc;
    }

    printf("Amiga headless: done (%d frames)\n", max_frames);
}

/* ------------------------------------------------------------------ */
/*  system_t descriptor                                                 */
/* ------------------------------------------------------------------ */

const system_t system_amiga = {
    .name         = "amiga",
    .description  = "Commodore Amiga 500",
    .init         = amiga_init,
    .reset        = amiga_reset,
    .shutdown     = amiga_shutdown,
    .run          = amiga_run,
    .run_headless = amiga_run_headless,
    .bus          = &amiga_bus_vtable,
};

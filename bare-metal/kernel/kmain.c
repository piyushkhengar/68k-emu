/* bare-metal/kernel/kmain.c
 * Bare-metal 68k emulator entry point.
 *
 * Genesis ROM: parses multiboot2 framebuffer info, runs game loop at ~60fps,
 *              blits VDP output to the VESA framebuffer, polls keyboard for
 *              joypad input.
 * Other ROM:   flat memory + interactive PS/2 stepper (VGA text mode).
 */

#include "bare.h"
#include "cpu.h"
#include "memory.h"
#include "genesis_bare.h"
#include "vesa.h"
#include "vdp.h"

/* ---- External declarations ----------------------------------------------- */
void serial_init(void);
void kbd_poll_joypad(void);
int  kbd_quit_pressed(void);
char kbd_wait(void);
char kbd_getchar(void);
int  kbd_ready(void);

extern const uint8_t  embedded_rom[];
extern const uint32_t embedded_rom_size;

/* ---- Multiboot2 tag parsing ---------------------------------------------- */
/*
 * Multiboot2 info structure layout:
 *   uint32_t total_size
 *   uint32_t reserved
 *   tags[] — each tag: uint32_t type, uint32_t size, then type-specific data
 *
 * Tag type 8 = framebuffer info:
 *   uint64_t framebuffer_addr
 *   uint32_t framebuffer_pitch
 *   uint32_t framebuffer_width
 *   uint32_t framebuffer_height
 *   uint8_t  framebuffer_bpp
 *   uint8_t  framebuffer_type  (1 = RGB linear)
 */

static void parse_multiboot2(uint32_t mb_info_phys)
{
    if (!mb_info_phys) return;

    uint8_t *p   = (uint8_t *)(uintptr_t)mb_info_phys;
    uint32_t total = *(uint32_t *)p;
    uint8_t *end = p + total;
    p += 8;  /* skip total_size + reserved */

    while (p < end) {
        uint32_t type = *(uint32_t *)p;
        uint32_t size = *(uint32_t *)(p + 4);

        if (type == 0) break;  /* terminating tag */

        if (type == 8 && size >= 24) {
            /* Framebuffer tag */
            uint64_t addr  = *(uint64_t *)(p + 8);
            uint32_t pitch = *(uint32_t *)(p + 16);
            uint32_t w     = *(uint32_t *)(p + 20);
            uint32_t h     = *(uint32_t *)(p + 24);
            uint8_t  bpp   = *(uint8_t  *)(p + 28);
            uint8_t  ftype = *(uint8_t  *)(p + 29);

            if (ftype == 1) /* RGB linear */
                vesa_configure(addr, pitch, w, h, bpp);
        }

        /* Tags are 8-byte aligned */
        p += (size + 7) & ~7u;
    }
}

/* ---- ROM detection ------------------------------------------------------- */

static int is_genesis_rom(const uint8_t *rom, uint32_t size)
{
    if (size < 0x200) return 0;
    return (rom[0x100] == 'S' && rom[0x101] == 'E' &&
            rom[0x102] == 'G' && rom[0x103] == 'A');
}

/* ---- Helpers ------------------------------------------------------------- */

static void dump_registers(void)
{
    kprintf("PC=%08lX  SR=%04X  cycles=%lu\n",
            (unsigned long)cpu.pc, cpu.sr, (unsigned long)cpu.cycles);
    for (int i = 0; i < 8; i++)
        kprintf("D%d=%08lX  A%d=%08lX\n",
                i, (unsigned long)cpu.d[i],
                i, (unsigned long)cpu.a[i]);
}

/* ---- Genesis game loop --------------------------------------------------- */

static void run_genesis_game(void)
{
    /* Print title from ROM header */
    kprintf("ROM: ");
    for (int i = 0x150; i < 0x180; i++) {
        char c = (char)embedded_rom[i];
        if (c == '\0') break;
        if (c != ' ' || i > 0x150) vga_putchar(c);
    }
    kprintf("\n");

    if (genesis_bare_init(embedded_rom, embedded_rom_size) < 0) {
        kprintf("Genesis init failed.\n");
        return;
    }

    if (vesa_available()) {
        vesa_clear();
        vesa_startup_check();   /* brief white corner — confirms blit works */
        kprintf("VESA framebuffer ready. Starting game...\n");
        kprintf("Controls: Arrow keys=D-pad  Z=A  X=B  C=C  Enter=Start  Esc=quit\n");
    } else {
        kprintf("No VESA framebuffer - running headless.\n");
        kprintf("Frame progress will print every 60 frames.\n");
        kprintf("Controls: Arrow keys=D-pad  Z=A  X=B  C=C  Enter=Start  Esc=quit\n");
    }

    /* Continuous game loop */
    int frame = 0;
    while (!cpu.halted && !kbd_quit_pressed()) {
        genesis_bare_run(1);  /* one frame */
        frame++;

        /* Blit to screen */
        if (vesa_available())
            vesa_blit();

        /* Poll keyboard → joypad state */
        kbd_poll_joypad();

        /* Heartbeat: every frame for first 100, then every 60. */
        if (frame <= 100 || frame % 60 == 0) {
            kprintf("F%4d PC=%08lX SR=%04X R1=%02X ipl=%d CRAM=%04X\n",
                    frame, (unsigned long)cpu.pc,
                    (unsigned)cpu.sr, (unsigned)vdp.regs[1],
                    (int)cpu_ipl,
                    (unsigned)vdp.cram[0]);
        }
    }

    kprintf("\nGame loop ended. Final state:\n");
    dump_registers();
}

/* ---- Flat-ROM interactive stepper --------------------------------------- */

static void run_flat(void)
{
    mem_init();
    mem_load_rom(embedded_rom, embedded_rom_size);
    cpu_init(CPU_MODEL_68000);
    cpu_reset();

    kprintf("CPU: 68000 initialised. PC=%08lX\n\n", (unsigned long)cpu.pc);
    kprintf("Controls: SPACE=step  R=regs  G=1000steps  Q=quit\n\n");

    for (;;) {
        char c = kbd_wait();
        if (c == ' ') {
            if (cpu.halted) { kprintf("CPU halted.\n"); continue; }
            int cyc = cpu_step();
            kprintf("PC=%08lX SR=%04X D0=%08lX cyc=%d\n",
                    (unsigned long)cpu.pc, cpu.sr,
                    (unsigned long)cpu.d[0], cyc);
        } else if (c == 'r' || c == 'R') {
            dump_registers();
        } else if (c == 'g' || c == 'G') {
            kprintf("Running 1000 steps...\n");
            for (int i = 0; i < 1000 && !cpu.halted; i++) cpu_step();
            kprintf("Done. ");
            dump_registers();
        } else if (c == 'q' || c == 'Q' || c == 27) {
            break;
        }
    }
}

/* ---- Entry point --------------------------------------------------------- */

void kmain(uint32_t mb_info_phys)
{
    serial_init();
    vga_clear();

    kprintf("68k-emu bare-metal\n");

    /* Parse multiboot2 info for VESA framebuffer */
    parse_multiboot2(mb_info_phys);

    kprintf("ROM: %lu bytes  ", (unsigned long)embedded_rom_size);

    if (is_genesis_rom(embedded_rom, embedded_rom_size)) {
        kprintf("[Genesis]\n");
        run_genesis_game();
    } else {
        kprintf("[flat]\n");
        run_flat();
    }

    kprintf("\n*** HALTED ***\n");
    for (;;) __asm__ volatile ("cli; hlt");
}

/* bare-metal/lib/irq.c
 * Minimal x86 PIC setup for SB16 audio — polling mode.
 *
 * We remap the 8259A PICs so hardware IRQ numbers don't collide with CPU
 * exception vectors, then keep interrupts DISABLED (IF=0).  The SB16 still
 * asserts IRQ5 after each half-buffer, but we never take an actual interrupt.
 * Instead, sb16_render_frame() calls irq_ack_sb16() each frame to:
 *   1. Read the SB16 ACK port (clears the SB16's internal interrupt flag)
 *   2. Send EOI to the PIC (clears the PIC's in-service bit)
 *
 * Why not use real interrupts?  Under QEMU's TCG (software CPU emulation on
 * Apple Silicon), the overhead of delivering ~43 hardware interrupts/second
 * to the emulated guest is high enough to stall the game loop entirely.
 * Polling avoids this by batching the ACK work into the main loop.
 */

#include "bare.h"
#include "irq_hw.h"

/* ---- I/O helpers --------------------------------------------------------- */

static inline void outb(uint16_t port, uint8_t val)
{
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t inb(uint16_t port)
{
    uint8_t val;
    __asm__ volatile ("inb %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}

static inline void io_wait(void) { outb(0x80, 0); }

/* ---- PIC remapping ------------------------------------------------------- */

#define PIC1_CMD   0x20
#define PIC1_DATA  0x21
#define PIC2_CMD   0xA0
#define PIC2_DATA  0xA1

static void pic_remap(void)
{
    outb(PIC1_CMD,  0x11); io_wait();
    outb(PIC2_CMD,  0x11); io_wait();

    outb(PIC1_DATA, 0x20); io_wait();   /* IRQ 0-7  → vectors 0x20-0x27 */
    outb(PIC2_DATA, 0x28); io_wait();   /* IRQ 8-15 → vectors 0x28-0x2F */

    outb(PIC1_DATA, 0x04); io_wait();
    outb(PIC2_DATA, 0x02); io_wait();

    outb(PIC1_DATA, 0x01); io_wait();
    outb(PIC2_DATA, 0x01); io_wait();

    /* Mask everything — interrupts stay disabled (IF=0), so masking is
     * just belt-and-suspenders to avoid stray signals. */
    outb(PIC1_DATA, 0xFF);
    outb(PIC2_DATA, 0xFF);
}

/* ---- Polling ACK for SB16 ------------------------------------------------ */

void irq_ack_sb16(void)
{
    (void)inb(0x22Fu);            /* clear SB16's 16-bit IRQ flag */
    (void)inb(0x22Eu);            /* clear SB16's 8-bit  IRQ flag */
    outb(PIC1_CMD, 0x20);         /* EOI to primary PIC           */
}

/* ---- Initialisation ------------------------------------------------------ */

void irq_init(void)
{
    pic_remap();
    /* Interrupts stay disabled (IF=0 from boot.asm).
     * No IDT is loaded — CPU exceptions triple-fault, which QEMU
     * reports in the monitor.  This is acceptable for development. */
    kprintf("IRQ: PIC remapped (polling mode, IF=0)\n");
}

#ifndef IRQ_HW_H
#define IRQ_HW_H

/* Remap PICs to avoid vector collisions.  Keeps interrupts disabled. */
void irq_init(void);

/* Poll-acknowledge a pending SB16 interrupt: reads the SB16 ACK port
 * and sends EOI to the primary PIC.  Call once per game-loop iteration. */
void irq_ack_sb16(void);

#endif

/* bare-metal/lib/serial.c
 * COM1 serial port driver for real hardware debugging.
 * 38400 baud, 8N1. Useful when VGA init fails on physical machines.
 */

#include "bare.h"

#define COM1_BASE 0x3F8

static inline void outb(uint16_t port, uint8_t val)
{
    __asm__ volatile ("outb %0, %1" :: "a"(val), "Nd"(port));
}

static inline uint8_t inb(uint16_t port)
{
    uint8_t val;
    __asm__ volatile ("inb %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}

void serial_init(void)
{
    outb(COM1_BASE + 1, 0x00); /* disable interrupts */
    outb(COM1_BASE + 3, 0x80); /* DLAB on */
    outb(COM1_BASE + 0, 0x03); /* 38400 baud low byte  (115200/3 = 38400) */
    outb(COM1_BASE + 1, 0x00); /* 38400 baud high byte */
    outb(COM1_BASE + 3, 0x03); /* 8 data bits, no parity, 1 stop bit; DLAB off */
    outb(COM1_BASE + 2, 0xC7); /* enable FIFO, clear, 14-byte threshold */
    outb(COM1_BASE + 4, 0x0B); /* DTR + RTS + OUT2 */
}

static int serial_transmit_empty(void)
{
    return (inb(COM1_BASE + 5) & 0x20) != 0;
}

void serial_putchar(char c)
{
    while (!serial_transmit_empty()) {}
    outb(COM1_BASE, (uint8_t)c);
}

void serial_puts(const char *s)
{
    while (*s) {
        if (*s == '\n') serial_putchar('\r');
        serial_putchar(*s++);
    }
}

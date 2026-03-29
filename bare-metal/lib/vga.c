/* bare-metal/lib/vga.c
 * VGA text mode driver: 80x25, attribute 0x0F (white on black).
 * Physical address 0xB8000.
 */

#include "bare.h"

#define VGA_ADDR  ((volatile uint16_t *)0xB8000)
#define VGA_COLS  80
#define VGA_ROWS  25
#define VGA_ATTR  0x0F00u  /* white on black */

static int vga_col = 0;
static int vga_row = 0;

static void vga_scroll(void)
{
    for (int r = 0; r < VGA_ROWS - 1; r++)
        for (int c = 0; c < VGA_COLS; c++)
            VGA_ADDR[r * VGA_COLS + c] = VGA_ADDR[(r + 1) * VGA_COLS + c];

    for (int c = 0; c < VGA_COLS; c++)
        VGA_ADDR[(VGA_ROWS - 1) * VGA_COLS + c] = VGA_ATTR | ' ';

    vga_row = VGA_ROWS - 1;
}

void vga_putchar(char c)
{
    if (c == '\n') {
        vga_col = 0;
        if (++vga_row >= VGA_ROWS)
            vga_scroll();
        return;
    }
    if (c == '\r') {
        vga_col = 0;
        return;
    }
    if (c == '\t') {
        int next = (vga_col + 8) & ~7;
        while (vga_col < next && vga_col < VGA_COLS)
            vga_putchar(' ');
        return;
    }
    VGA_ADDR[vga_row * VGA_COLS + vga_col] = VGA_ATTR | (uint8_t)c;
    if (++vga_col >= VGA_COLS) {
        vga_col = 0;
        if (++vga_row >= VGA_ROWS)
            vga_scroll();
    }
}

void vga_clear(void)
{
    for (int i = 0; i < VGA_ROWS * VGA_COLS; i++)
        VGA_ADDR[i] = VGA_ATTR | ' ';
    vga_col = 0;
    vga_row = 0;
}

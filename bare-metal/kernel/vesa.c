/* bare-metal/kernel/vesa.c
 * Blits the VDP framebuffer (320x224 ARGB8888) to a VESA linear framebuffer.
 * Uses integer scaling: computes the largest integer scale that fits the screen,
 * then centres the image.
 */

#include "bare.h"
#include "vdp.h"

/* Set by kmain after parsing the multiboot2 framebuffer tag */
static volatile uint8_t  *fb_addr  = 0;
static uint32_t           fb_pitch = 0;
static uint32_t           fb_w     = 0;
static uint32_t           fb_h     = 0;
static uint32_t           fb_bpp   = 0;   /* bits per pixel */

/* Computed once by vesa_configure() */
static uint32_t scale   = 1;
static uint32_t off_x   = 0;
static uint32_t off_y   = 0;

#define VDP_W 320
#define VDP_H 224

void vesa_configure(uint64_t addr, uint32_t pitch, uint32_t w, uint32_t h, uint8_t bpp)
{
    fb_addr  = (volatile uint8_t *)(uintptr_t)addr;
    fb_pitch = pitch;
    fb_w     = w;
    fb_h     = h;
    fb_bpp   = bpp;

    /* Largest integer scale that fits both dimensions */
    uint32_t sx = w / VDP_W;
    uint32_t sy = h / VDP_H;
    scale = sx < sy ? sx : sy;
    if (scale == 0) scale = 1;

    /* Centre the scaled image */
    off_x = (w - VDP_W * scale) / 2;
    off_y = (h - VDP_H * scale) / 2;

    kprintf("VESA: %lux%lu bpp=%lu scale=%lux  image=%lux%lu at (%lu,%lu)\n",
            (unsigned long)w, (unsigned long)h, (unsigned long)bpp,
            (unsigned long)scale,
            (unsigned long)(VDP_W * scale), (unsigned long)(VDP_H * scale),
            (unsigned long)off_x, (unsigned long)off_y);
}

int vesa_available(void)
{
    return fb_addr != 0 && fb_bpp == 32;
}

/* Write a small 32×8 white rectangle in the very top-left corner.
 * Call once after vesa_clear() to confirm framebuffer writes reach the screen.
 * It disappears once the first vesa_blit() overwrites it. */
void vesa_startup_check(void)
{
    if (!fb_addr || fb_bpp != 32) return;
    for (uint32_t y = 0; y < 8 && y < fb_h; y++) {
        uint32_t *row = (uint32_t *)(fb_addr + (uint64_t)y * fb_pitch);
        for (uint32_t x = 0; x < 32 && x < fb_w; x++)
            row[x] = 0xFFFFFFFF;  /* white */
    }
}

/* Clear the entire framebuffer to black */
void vesa_clear(void)
{
    if (!fb_addr) return;
    for (uint32_t y = 0; y < fb_h; y++) {
        uint32_t *row = (uint32_t *)(fb_addr + (uint64_t)y * fb_pitch);
        for (uint32_t x = 0; x < fb_w; x++)
            row[x] = 0;
    }
}

/* Blit VDP framebuffer to screen */
void vesa_blit(void)
{
    if (!fb_addr || fb_bpp != 32) return;

    const uint32_t *src = vdp.framebuffer;

    for (uint32_t sy = 0; sy < VDP_H; sy++) {
        const uint32_t *src_row = src + sy * VDP_W;

        for (uint32_t dy = 0; dy < scale; dy++) {
            uint32_t screen_y = off_y + sy * scale + dy;
            if (screen_y >= fb_h) break;

            uint32_t *dst = (uint32_t *)(fb_addr + (uint64_t)screen_y * fb_pitch);

            for (uint32_t sx = 0; sx < VDP_W; sx++) {
                uint32_t pixel = src_row[sx];
                uint32_t screen_x = off_x + sx * scale;

                for (uint32_t dx = 0; dx < scale; dx++) {
                    if (screen_x + dx < fb_w)
                        dst[screen_x + dx] = pixel;
                }
            }
        }
    }
}

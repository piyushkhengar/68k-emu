#ifndef VESA_H
#define VESA_H

#include <stdint.h>

void vesa_configure(uint64_t addr, uint32_t pitch, uint32_t w, uint32_t h, uint8_t bpp);
int  vesa_available(void);
void vesa_clear(void);
void vesa_startup_check(void);
void vesa_blit(void);

#endif

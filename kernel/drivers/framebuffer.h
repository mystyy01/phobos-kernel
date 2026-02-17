#ifndef FRAMEBUFFER_H
#define FRAMEBUFFER_H

#include <stdint.h>

void fb_init(void);
void fb_putpixel(int x, int y, uint32_t colour);
int fb_width(void);
int fb_height(void);
int fb_bpp(void);
uint64_t fb_base_addr(void);

#endif

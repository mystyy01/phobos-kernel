#ifndef FRAMEBUFFER_H
#define FRAMEBUFFER_H

#include <stdint.h>

void fb_init(void);
void fb_set_surface(void *base, uint16_t new_width, uint16_t new_height, uint8_t new_bpp);
void fb_putpixel(int x, int y, uint32_t colour);
int fb_width(void);
int fb_height(void);
int fb_bpp(void);
uint64_t fb_base_addr(void);
void fb_present_buffer(const void *src, uint64_t size);
void fb_present_buffer_rect(const void *src, int x, int y, int w, int h);

#endif

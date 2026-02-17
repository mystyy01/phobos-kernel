#include "framebuffer.h"
static uint8_t *fb;
static uint16_t width;
static uint16_t height;
static uint8_t bpp;
void fb_init(void){
    fb = (uint8_t *)(uintptr_t)(*(uint32_t *)0x5028);
    width = *(uint16_t *)0x5012;
    height = *(uint16_t *)0x5014;
    bpp = *(uint8_t *)0x5019;
}
void fb_putpixel(int x, int y, uint32_t colour){
    if (!fb || width == 0 || height == 0) {
        return;
    }
    if (x < 0 || y < 0 || x >= width || y >= height) {
        return;
    }

    int bytes_per_pixel = bpp / 8;
    int pos = (y * width + x) * bytes_per_pixel;

    if (bpp == 16) {
        uint8_t r = (uint8_t)((colour >> 16) & 0xFF);
        uint8_t g = (uint8_t)((colour >> 8) & 0xFF);
        uint8_t b = (uint8_t)(colour & 0xFF);
        uint16_t rgb565 = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
        *(uint16_t *)(fb + pos) = rgb565;
        return;
    }

    if (bpp == 24) {
        fb[pos + 0] = (uint8_t)(colour & 0xFF);
        fb[pos + 1] = (uint8_t)((colour >> 8) & 0xFF);
        fb[pos + 2] = (uint8_t)((colour >> 16) & 0xFF);
        return;
    }

    if (bpp == 32) {
        *(uint32_t *)(fb + pos) = colour;
    }
}
int fb_width(void){
    return width;
}
int fb_height(void){
    return height;
}
int fb_bpp(void){
    return bpp;
}
uint64_t fb_base_addr(void){
    return (uint64_t)(uintptr_t)fb;
}

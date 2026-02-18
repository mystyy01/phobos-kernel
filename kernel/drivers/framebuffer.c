#include "framebuffer.h"
#include "virtio_gpu.h"
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
void fb_present_buffer(const void *src, uint64_t size){
    if (!fb || !src || size == 0) return;
    if (virtio_gpu_ready()) {
        if (virtio_gpu_present_full(src, (uint32_t)width, (uint32_t)height, (uint32_t)bpp)) {
            return;
        }
    }
    uint64_t qwords = size / 8;
    const void *s = src;
    void *d = (void *)fb;
    __asm__ volatile (
        "rep movsq"
        : "+S"(s), "+D"(d), "+c"(qwords)
        :
        : "memory"
    );
    // Copy remaining bytes
    uint64_t rem = size & 7;
    if (rem) {
        const uint8_t *sb = (const uint8_t *)s;
        uint8_t *db = (uint8_t *)d;
        for (uint64_t i = 0; i < rem; i++) db[i] = sb[i];
    }
}

void fb_present_buffer_rect(const void *src, int x, int y, int w, int h){
    if (!fb || !src) return;
    if (w <= 0 || h <= 0) return;
    if (virtio_gpu_ready()) {
        if (virtio_gpu_present_rect(src, (uint32_t)width, (uint32_t)height, (uint32_t)bpp, x, y, w, h)) {
            return;
        }
    }

    int cx = x;
    int cy = y;
    int cw = w;
    int ch = h;

    if (cx < 0) {
        cw += cx;
        cx = 0;
    }
    if (cy < 0) {
        ch += cy;
        cy = 0;
    }
    if (cw <= 0 || ch <= 0) return;
    if (cx >= (int)width || cy >= (int)height) return;

    if (cx + cw > (int)width)  cw = (int)width - cx;
    if (cy + ch > (int)height) ch = (int)height - cy;
    if (cw <= 0 || ch <= 0) return;

    int bytes_per_pixel = bpp / 8;
    if (bytes_per_pixel <= 0) return;

    uint64_t stride = (uint64_t)width * (uint64_t)bytes_per_pixel;
    uint64_t row_bytes = (uint64_t)cw * (uint64_t)bytes_per_pixel;

    const uint8_t *srow = (const uint8_t *)src + ((uint64_t)cy * stride) + ((uint64_t)cx * (uint64_t)bytes_per_pixel);
    uint8_t *drow = fb + ((uint64_t)cy * stride) + ((uint64_t)cx * (uint64_t)bytes_per_pixel);

    for (int row = 0; row < ch; row++) {
        uint64_t qwords = row_bytes / 8;
        const void *s = srow;
        void *d = drow;

        __asm__ volatile (
            "rep movsq"
            : "+S"(s), "+D"(d), "+c"(qwords)
            :
            : "memory"
        );

        uint64_t rem = row_bytes & 7;
        if (rem) {
            const uint8_t *sb = (const uint8_t *)s;
            uint8_t *db = (uint8_t *)d;
            for (uint64_t i = 0; i < rem; i++) db[i] = sb[i];
        }

        srow += stride;
        drow += stride;
    }
}

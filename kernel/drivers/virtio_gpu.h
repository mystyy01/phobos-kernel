#ifndef VIRTIO_GPU_H
#define VIRTIO_GPU_H

#include <stdint.h>

void virtio_gpu_init(void);
int virtio_gpu_ready(void);
int virtio_gpu_present_full(const void *src, uint32_t src_width, uint32_t src_height, uint32_t src_bpp);
int virtio_gpu_present_rect(const void *src, uint32_t src_width, uint32_t src_height, uint32_t src_bpp,
                            int x, int y, int w, int h);

#endif

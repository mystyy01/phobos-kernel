#include "virtio_gpu.h"
#include "pci.h"
#include "../paging.h"
#include "../pmm.h"
#include <stdint.h>

#define VIRTIO_VENDOR_ID           0x1AF4
#define VIRTIO_GPU_DEVICE_MODERN   0x1050
#define VIRTIO_GPU_DEVICE_LEGACY   0x1005

// Legacy virtio-pci register layout
#define VIRTIO_PCI_HOST_FEATURES   0x00
#define VIRTIO_PCI_GUEST_FEATURES  0x04
#define VIRTIO_PCI_QUEUE_PFN       0x08
#define VIRTIO_PCI_QUEUE_NUM       0x0C
#define VIRTIO_PCI_QUEUE_SEL       0x0E
#define VIRTIO_PCI_QUEUE_NOTIFY    0x10
#define VIRTIO_PCI_STATUS          0x12
#define VIRTIO_PCI_ISR             0x13

#define VIRTIO_STATUS_ACKNOWLEDGE  0x01
#define VIRTIO_STATUS_DRIVER       0x02
#define VIRTIO_STATUS_DRIVER_OK    0x04
#define VIRTIO_STATUS_FEATURES_OK  0x08
#define VIRTIO_STATUS_FAILED       0x80

#define VIRTIO_GPU_QUEUE_CONTROL   0
#define VIRTIO_GPU_QUEUE_CURSOR    1

#define VRING_DESC_F_NEXT          1
#define VRING_DESC_F_WRITE         2

// Virtio-gpu control commands/responses
#define VIRTIO_GPU_CMD_GET_DISPLAY_INFO    0x0100
#define VIRTIO_GPU_CMD_RESOURCE_CREATE_2D  0x0101
#define VIRTIO_GPU_CMD_SET_SCANOUT         0x0103
#define VIRTIO_GPU_CMD_RESOURCE_FLUSH      0x0104
#define VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D 0x0105
#define VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING 0x0106
#define VIRTIO_GPU_CMD_RESOURCE_DETACH_BACKING 0x0107
#define VIRTIO_GPU_RESP_OK_DISPLAY_INFO    0x1101
#define VIRTIO_GPU_RESP_OK_NODATA          0x1100

#define VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM   2

struct virtio_gpu_rect {
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
} __attribute__((packed));

struct virtio_gpu_ctrl_hdr {
    uint32_t type;
    uint32_t flags;
    uint64_t fence_id;
    uint32_t ctx_id;
    uint32_t padding;
} __attribute__((packed));

struct virtio_gpu_display_one {
    struct virtio_gpu_rect r;
    uint32_t enabled;
    uint32_t flags;
} __attribute__((packed));

struct virtio_gpu_resp_display_info {
    struct virtio_gpu_ctrl_hdr hdr;
    struct virtio_gpu_display_one pmodes[16];
} __attribute__((packed));

struct virtio_gpu_resource_create_2d {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32_t resource_id;
    uint32_t format;
    uint32_t width;
    uint32_t height;
} __attribute__((packed));

struct virtio_gpu_resource_attach_backing {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32_t resource_id;
    uint32_t nr_entries;
} __attribute__((packed));

struct virtio_gpu_mem_entry {
    uint64_t addr;
    uint32_t length;
    uint32_t padding;
} __attribute__((packed));

struct virtio_gpu_set_scanout {
    struct virtio_gpu_ctrl_hdr hdr;
    struct virtio_gpu_rect r;
    uint32_t scanout_id;
    uint32_t resource_id;
} __attribute__((packed));

struct virtio_gpu_transfer_to_host_2d {
    struct virtio_gpu_ctrl_hdr hdr;
    struct virtio_gpu_rect r;
    uint64_t offset;
    uint32_t resource_id;
    uint32_t padding;
} __attribute__((packed));

struct virtio_gpu_resource_flush {
    struct virtio_gpu_ctrl_hdr hdr;
    struct virtio_gpu_rect r;
    uint32_t resource_id;
    uint32_t padding;
} __attribute__((packed));

struct virtq_desc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
} __attribute__((packed));

struct virtq_avail {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[];
} __attribute__((packed));

struct virtq_used_elem {
    uint32_t id;
    uint32_t len;
} __attribute__((packed));

struct virtq_used {
    uint16_t flags;
    uint16_t idx;
    struct virtq_used_elem ring[];
} __attribute__((packed));

struct virtio_queue {
    uint16_t size;
    uint64_t bytes;
    uint8_t *mem;
    struct virtq_desc *desc;
    struct virtq_avail *avail;
    struct virtq_used *used;
    uint16_t next_avail_idx;
    uint16_t last_used_idx;
};

static int g_ready = 0;
static int g_use_io = 0;
static uint16_t g_io_base = 0;
static volatile uint8_t *g_mmio = 0;
static struct virtio_queue g_ctrlq;
static struct virtio_queue g_cursorq;
static struct virtio_gpu_ctrl_hdr g_req_hdr __attribute__((aligned(16)));
static struct virtio_gpu_resp_display_info g_disp_info __attribute__((aligned(16)));
static uint32_t g_scanout_id;
static uint32_t g_scanout_w;
static uint32_t g_scanout_h;
static uint32_t g_resource_id = 1;
static uint8_t *g_resource_backing;
static uint64_t g_resource_backing_bytes;

static inline uint8_t inb(uint16_t port) {
    uint8_t value;
    __asm__ volatile ("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static inline uint16_t inw(uint16_t port) {
    uint16_t value;
    __asm__ volatile ("inw %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static inline uint32_t inl(uint16_t port) {
    uint32_t value;
    __asm__ volatile ("inl %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static inline void outb(uint16_t port, uint8_t value) {
    __asm__ volatile ("outb %0, %1" : : "a"(value), "Nd"(port));
}

static inline void outw(uint16_t port, uint16_t value) {
    __asm__ volatile ("outw %0, %1" : : "a"(value), "Nd"(port));
}

static inline void outl(uint16_t port, uint32_t value) {
    __asm__ volatile ("outl %0, %1" : : "a"(value), "Nd"(port));
}

static void dbg_char(char c) {
    outb(0xE9, (uint8_t)c);
}

static void dbg_str(const char *s) {
    while (*s) dbg_char(*s++);
}

static void dbg_hex32(uint32_t val) {
    const char *hex = "0123456789ABCDEF";
    for (int i = 7; i >= 0; i--) {
        dbg_char(hex[(val >> (i * 4)) & 0xF]);
    }
}

static uint8_t vgpu_read8(uint16_t off) {
    if (g_use_io) return inb((uint16_t)(g_io_base + off));
    return *(volatile uint8_t *)(g_mmio + off);
}

static uint16_t vgpu_read16(uint16_t off) {
    if (g_use_io) return inw((uint16_t)(g_io_base + off));
    return *(volatile uint16_t *)(g_mmio + off);
}

static uint32_t vgpu_read32(uint16_t off) {
    if (g_use_io) return inl((uint16_t)(g_io_base + off));
    return *(volatile uint32_t *)(g_mmio + off);
}

static void vgpu_write8(uint16_t off, uint8_t value) {
    if (g_use_io) outb((uint16_t)(g_io_base + off), value);
    else *(volatile uint8_t *)(g_mmio + off) = value;
}

static void vgpu_write16(uint16_t off, uint16_t value) {
    if (g_use_io) outw((uint16_t)(g_io_base + off), value);
    else *(volatile uint16_t *)(g_mmio + off) = value;
}

static void vgpu_write32(uint16_t off, uint32_t value) {
    if (g_use_io) outl((uint16_t)(g_io_base + off), value);
    else *(volatile uint32_t *)(g_mmio + off) = value;
}

static uint64_t align_up_u64(uint64_t value, uint64_t align) {
    return (value + align - 1) & ~(align - 1);
}

static int map_mmio_identity(uint64_t phys, uint64_t size) {
    uint64_t start = phys & ~0xFFFULL;
    uint64_t end = align_up_u64(phys + size, 0x1000);
    uint64_t *kpml4 = paging_kernel_pml4();
    for (uint64_t p = start; p < end; p += 0x1000) {
        if (paging_map_kernel_page(kpml4, p, p, PAGE_PRESENT | PAGE_WRITABLE) < 0) {
            return 0;
        }
    }
    return 1;
}

static uint8_t *alloc_contiguous_pages(int pages) {
    if (pages <= 0) return 0;
    uint8_t *first = (uint8_t *)pmm_alloc_page();
    if (!first) return 0;
    if (paging_map_kernel_page(paging_kernel_pml4(), (uint64_t)(uintptr_t)first,
                               (uint64_t)(uintptr_t)first, PAGE_PRESENT | PAGE_WRITABLE) < 0) {
        return 0;
    }

    for (int i = 1; i < pages; i++) {
        uint8_t *next = (uint8_t *)pmm_alloc_page();
        if (!next) return 0;
        if (paging_map_kernel_page(paging_kernel_pml4(), (uint64_t)(uintptr_t)next,
                                   (uint64_t)(uintptr_t)next, PAGE_PRESENT | PAGE_WRITABLE) < 0) {
            return 0;
        }
        if (next != first + (i * 4096)) {
            return 0;
        }
    }
    return first;
}

static void mem_zero(void *ptr, uint64_t n) {
    uint8_t *p = (uint8_t *)ptr;
    for (uint64_t i = 0; i < n; i++) p[i] = 0;
}

static int virtio_queue_setup_legacy(uint16_t queue_id, uint16_t requested_size, struct virtio_queue *out) {
    if (!out) return 0;

    vgpu_write16(VIRTIO_PCI_QUEUE_SEL, queue_id);
    uint16_t max_size = vgpu_read16(VIRTIO_PCI_QUEUE_NUM);
    if (max_size == 0) {
        return 0;
    }

    uint16_t qsz = requested_size;
    if (qsz == 0 || qsz > max_size) qsz = max_size;
    vgpu_write16(VIRTIO_PCI_QUEUE_NUM, qsz);

    uint64_t desc_bytes = (uint64_t)qsz * sizeof(struct virtq_desc);
    uint64_t avail_bytes = 4 + ((uint64_t)qsz * 2) + 2;
    uint64_t used_off = align_up_u64(desc_bytes + avail_bytes, 4096);
    uint64_t used_bytes = 4 + ((uint64_t)qsz * sizeof(struct virtq_used_elem)) + 2;
    uint64_t total_bytes = used_off + used_bytes;
    int pages = (int)align_up_u64(total_bytes, 4096) / 4096;

    uint8_t *queue_mem = alloc_contiguous_pages(pages);
    if (!queue_mem) {
        return 0;
    }
    mem_zero(queue_mem, (uint64_t)pages * 4096);

    uint64_t pfn = ((uint64_t)(uintptr_t)queue_mem) >> 12;
    vgpu_write32(VIRTIO_PCI_QUEUE_PFN, (uint32_t)pfn);
    if (vgpu_read32(VIRTIO_PCI_QUEUE_PFN) != (uint32_t)pfn) {
        return 0;
    }

    out->size = qsz;
    out->bytes = (uint64_t)pages * 4096;
    out->mem = queue_mem;
    out->desc = (struct virtq_desc *)(queue_mem);
    out->avail = (struct virtq_avail *)(queue_mem + desc_bytes);
    out->used = (struct virtq_used *)(queue_mem + used_off);
    out->next_avail_idx = 0;
    out->last_used_idx = 0;
    return 1;
}

static int virtio_gpu_submit_sync(struct virtio_queue *q, uint16_t queue_id,
                                  void *req, uint32_t req_len,
                                  void *resp, uint32_t resp_len) {
    if (!q || !req || !resp || q->size < 2) return 0;

    uint16_t head = 0;
    q->desc[0].addr = (uint64_t)(uintptr_t)req;
    q->desc[0].len = req_len;
    q->desc[0].flags = VRING_DESC_F_NEXT;
    q->desc[0].next = 1;

    q->desc[1].addr = (uint64_t)(uintptr_t)resp;
    q->desc[1].len = resp_len;
    q->desc[1].flags = VRING_DESC_F_WRITE;
    q->desc[1].next = 0;

    uint16_t slot = (uint16_t)(q->next_avail_idx % q->size);
    q->avail->ring[slot] = head;
    __asm__ volatile ("" : : : "memory");
    q->next_avail_idx++;
    q->avail->idx = q->next_avail_idx;

    vgpu_write16(VIRTIO_PCI_QUEUE_NOTIFY, queue_id);

    uint32_t timeout = 20000000;
    while (q->used->idx == q->last_used_idx && timeout--) {
        __asm__ volatile ("pause");
    }
    if (q->used->idx == q->last_used_idx) {
        return 0;
    }

    q->last_used_idx = q->used->idx;
    (void)vgpu_read8(VIRTIO_PCI_ISR);
    return 1;
}

static int virtio_gpu_submit_ctrl(void *req, uint32_t req_len, void *resp, uint32_t resp_len) {
    return virtio_gpu_submit_sync(&g_ctrlq, VIRTIO_GPU_QUEUE_CONTROL, req, req_len, resp, resp_len);
}

static int virtio_gpu_expect_nodata(const char *tag, uint32_t type) {
    if (type == VIRTIO_GPU_RESP_OK_NODATA) return 1;
    dbg_str("[virtio-gpu] ");
    dbg_str(tag);
    dbg_str(" bad response=");
    dbg_hex32(type);
    dbg_char('\n');
    return 0;
}

static int virtio_gpu_probe_display_info(void) {
    mem_zero(&g_req_hdr, sizeof(g_req_hdr));
    mem_zero(&g_disp_info, sizeof(g_disp_info));

    g_req_hdr.type = VIRTIO_GPU_CMD_GET_DISPLAY_INFO;
    if (!virtio_gpu_submit_sync(&g_ctrlq, VIRTIO_GPU_QUEUE_CONTROL,
                                &g_req_hdr, sizeof(g_req_hdr),
                                &g_disp_info, sizeof(g_disp_info))) {
        dbg_str("[virtio-gpu] GET_DISPLAY_INFO submit failed\n");
        return 0;
    }

    if (g_disp_info.hdr.type != VIRTIO_GPU_RESP_OK_DISPLAY_INFO) {
        dbg_str("[virtio-gpu] GET_DISPLAY_INFO bad response=");
        dbg_hex32(g_disp_info.hdr.type);
        dbg_char('\n');
        return 0;
    }

    // Pick first enabled scanout for bring-up.
    for (int i = 0; i < 16; i++) {
        if (!g_disp_info.pmodes[i].enabled) continue;
        g_scanout_id = (uint32_t)i;
        g_scanout_w = g_disp_info.pmodes[i].r.width;
        g_scanout_h = g_disp_info.pmodes[i].r.height;
        dbg_str("[virtio-gpu] scanout ");
        dbg_char((char)('0' + (i % 10)));
        dbg_str(" ");
        dbg_hex32(g_scanout_w);
        dbg_char('x');
        dbg_hex32(g_scanout_h);
        dbg_char('\n');
        return 1;
    }

    dbg_str("[virtio-gpu] no enabled scanout in response\n");
    return 0;
}

static void virtio_gpu_fill_test_pattern(uint8_t *buf, uint32_t width, uint32_t height) {
    if (!buf || width == 0 || height == 0) return;
    uint32_t *pix = (uint32_t *)buf;
    for (uint32_t y = 0; y < height; y++) {
        for (uint32_t x = 0; x < width; x++) {
            uint32_t r = (x * 255U) / (width ? width : 1);
            uint32_t g = (y * 255U) / (height ? height : 1);
            uint32_t b = ((x ^ y) & 0xFFU);
            pix[y * width + x] = (r << 16) | (g << 8) | b;
        }
    }
}

static int virtio_gpu_create_2d_resource(uint32_t resource_id, uint32_t width, uint32_t height) {
    struct virtio_gpu_resource_create_2d req;
    struct virtio_gpu_ctrl_hdr resp;
    mem_zero(&req, sizeof(req));
    mem_zero(&resp, sizeof(resp));

    req.hdr.type = VIRTIO_GPU_CMD_RESOURCE_CREATE_2D;
    req.resource_id = resource_id;
    req.format = VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM;
    req.width = width;
    req.height = height;

    if (!virtio_gpu_submit_ctrl(&req, sizeof(req), &resp, sizeof(resp))) {
        dbg_str("[virtio-gpu] RESOURCE_CREATE_2D submit failed\n");
        return 0;
    }
    return virtio_gpu_expect_nodata("RESOURCE_CREATE_2D", resp.type);
}

static int virtio_gpu_attach_backing(uint32_t resource_id, void *backing, uint32_t length) {
    struct {
        struct virtio_gpu_resource_attach_backing req;
        struct virtio_gpu_mem_entry entry;
    } __attribute__((packed)) msg;
    struct virtio_gpu_ctrl_hdr resp;
    mem_zero(&msg, sizeof(msg));
    mem_zero(&resp, sizeof(resp));

    msg.req.hdr.type = VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING;
    msg.req.resource_id = resource_id;
    msg.req.nr_entries = 1;
    msg.entry.addr = (uint64_t)(uintptr_t)backing;
    msg.entry.length = length;

    if (!virtio_gpu_submit_ctrl(&msg, sizeof(msg), &resp, sizeof(resp))) {
        dbg_str("[virtio-gpu] ATTACH_BACKING submit failed\n");
        return 0;
    }
    return virtio_gpu_expect_nodata("ATTACH_BACKING", resp.type);
}

static int virtio_gpu_set_scanout(uint32_t scanout_id, uint32_t resource_id, uint32_t width, uint32_t height) {
    struct virtio_gpu_set_scanout req;
    struct virtio_gpu_ctrl_hdr resp;
    mem_zero(&req, sizeof(req));
    mem_zero(&resp, sizeof(resp));

    req.hdr.type = VIRTIO_GPU_CMD_SET_SCANOUT;
    req.r.x = 0;
    req.r.y = 0;
    req.r.width = width;
    req.r.height = height;
    req.scanout_id = scanout_id;
    req.resource_id = resource_id;

    if (!virtio_gpu_submit_ctrl(&req, sizeof(req), &resp, sizeof(resp))) {
        dbg_str("[virtio-gpu] SET_SCANOUT submit failed\n");
        return 0;
    }
    return virtio_gpu_expect_nodata("SET_SCANOUT", resp.type);
}

static int virtio_gpu_transfer_to_host(uint32_t resource_id, uint32_t width, uint32_t height) {
    struct virtio_gpu_transfer_to_host_2d req;
    struct virtio_gpu_ctrl_hdr resp;
    mem_zero(&req, sizeof(req));
    mem_zero(&resp, sizeof(resp));

    req.hdr.type = VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D;
    req.r.x = 0;
    req.r.y = 0;
    req.r.width = width;
    req.r.height = height;
    req.offset = 0;
    req.resource_id = resource_id;

    if (!virtio_gpu_submit_ctrl(&req, sizeof(req), &resp, sizeof(resp))) {
        dbg_str("[virtio-gpu] TRANSFER_TO_HOST_2D submit failed\n");
        return 0;
    }
    return virtio_gpu_expect_nodata("TRANSFER_TO_HOST_2D", resp.type);
}

static int virtio_gpu_flush_resource(uint32_t resource_id, uint32_t width, uint32_t height) {
    struct virtio_gpu_resource_flush req;
    struct virtio_gpu_ctrl_hdr resp;
    mem_zero(&req, sizeof(req));
    mem_zero(&resp, sizeof(resp));

    req.hdr.type = VIRTIO_GPU_CMD_RESOURCE_FLUSH;
    req.r.x = 0;
    req.r.y = 0;
    req.r.width = width;
    req.r.height = height;
    req.resource_id = resource_id;

    if (!virtio_gpu_submit_ctrl(&req, sizeof(req), &resp, sizeof(resp))) {
        dbg_str("[virtio-gpu] RESOURCE_FLUSH submit failed\n");
        return 0;
    }
    return virtio_gpu_expect_nodata("RESOURCE_FLUSH", resp.type);
}

static int virtio_gpu_setup_boot_framebuffer(void) {
    if (g_scanout_w == 0 || g_scanout_h == 0) {
        return 0;
    }

    uint64_t bytes = (uint64_t)g_scanout_w * (uint64_t)g_scanout_h * 4ULL;
    int pages = (int)align_up_u64(bytes, 4096) / 4096;
    uint8_t *backing = alloc_contiguous_pages(pages);
    if (!backing) {
        dbg_str("[virtio-gpu] backing alloc failed\n");
        return 0;
    }
    g_resource_backing = backing;
    g_resource_backing_bytes = (uint64_t)pages * 4096ULL;
    mem_zero(g_resource_backing, g_resource_backing_bytes);
    virtio_gpu_fill_test_pattern(g_resource_backing, g_scanout_w, g_scanout_h);

    if (!virtio_gpu_create_2d_resource(g_resource_id, g_scanout_w, g_scanout_h)) return 0;
    if (!virtio_gpu_attach_backing(g_resource_id, g_resource_backing, (uint32_t)bytes)) return 0;
    if (!virtio_gpu_set_scanout(g_scanout_id, g_resource_id, g_scanout_w, g_scanout_h)) return 0;
    if (!virtio_gpu_transfer_to_host(g_resource_id, g_scanout_w, g_scanout_h)) return 0;
    if (!virtio_gpu_flush_resource(g_resource_id, g_scanout_w, g_scanout_h)) return 0;

    dbg_str("[virtio-gpu] boot frame submitted\n");
    return 1;
}

void virtio_gpu_init(void) {
    struct pci_device dev;
    int found = pci_find_device_by_id(VIRTIO_VENDOR_ID, VIRTIO_GPU_DEVICE_MODERN, &dev);
    if (!found) {
        found = pci_find_device_by_id(VIRTIO_VENDOR_ID, VIRTIO_GPU_DEVICE_LEGACY, &dev);
    }
    if (!found) {
        dbg_str("[virtio-gpu] not found\n");
        return;
    }

    // Enable PCI command bits needed by virtio devices.
    uint16_t cmd = pci_config_read16(dev.bus, dev.slot, dev.func, 0x04);
    cmd |= (uint16_t)(0x0001 | 0x0002 | 0x0004); // IO, MEM, BUS MASTER
    pci_config_write16(dev.bus, dev.slot, dev.func, 0x04, cmd);

    uint32_t bar0 = pci_config_read32(dev.bus, dev.slot, dev.func, 0x10);
    if (bar0 == 0 || bar0 == 0xFFFFFFFFu) {
        dbg_str("[virtio-gpu] BAR0 invalid\n");
        return;
    }

    if (bar0 & 0x1) {
        g_use_io = 1;
        g_io_base = (uint16_t)(bar0 & ~0x3u);
        dbg_str("[virtio-gpu] IO base=");
        dbg_hex32(g_io_base);
        dbg_char('\n');
    } else {
        g_use_io = 0;
        uint64_t phys = (uint64_t)(bar0 & ~0xFu);
        if (!map_mmio_identity(phys, 0x1000)) {
            dbg_str("[virtio-gpu] MMIO map failed\n");
            return;
        }
        g_mmio = (volatile uint8_t *)(uintptr_t)phys;
        dbg_str("[virtio-gpu] MMIO base=");
        dbg_hex32((uint32_t)phys);
        dbg_char('\n');
    }

    // Legacy virtio status handshake.
    vgpu_write8(VIRTIO_PCI_STATUS, 0);
    uint8_t status = 0;
    status |= VIRTIO_STATUS_ACKNOWLEDGE;
    vgpu_write8(VIRTIO_PCI_STATUS, status);
    status |= VIRTIO_STATUS_DRIVER;
    vgpu_write8(VIRTIO_PCI_STATUS, status);

    uint32_t host_features = vgpu_read32(VIRTIO_PCI_HOST_FEATURES);
    (void)host_features;
    vgpu_write32(VIRTIO_PCI_GUEST_FEATURES, 0);

    if (!virtio_queue_setup_legacy(VIRTIO_GPU_QUEUE_CONTROL, 8, &g_ctrlq)) {
        status |= VIRTIO_STATUS_FAILED;
        vgpu_write8(VIRTIO_PCI_STATUS, status);
        dbg_str("[virtio-gpu] control queue setup failed\n");
        return;
    }
    if (!virtio_queue_setup_legacy(VIRTIO_GPU_QUEUE_CURSOR, 8, &g_cursorq)) {
        status |= VIRTIO_STATUS_FAILED;
        vgpu_write8(VIRTIO_PCI_STATUS, status);
        dbg_str("[virtio-gpu] cursor queue setup failed\n");
        return;
    }

    status |= VIRTIO_STATUS_DRIVER_OK;
    vgpu_write8(VIRTIO_PCI_STATUS, status);

    if (!virtio_gpu_probe_display_info()) {
        dbg_str("[virtio-gpu] command path not ready yet\n");
    }

    if (!virtio_gpu_setup_boot_framebuffer()) {
        dbg_str("[virtio-gpu] boot framebuffer setup failed\n");
    }

    g_ready = 1;
    dbg_str("[virtio-gpu] ready (legacy init)\n");
}

int virtio_gpu_ready(void) {
    return g_ready;
}

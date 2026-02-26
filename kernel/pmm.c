#include "pmm.h"

// Simple bitmap-based physical page allocator.
// Assumes a contiguous usable range; no NUMA; no highmem.

#define MAX_PAGES (1024 * 1024)  // supports up to 4 GiB with 4 KiB pages

static uint64_t bitmap[MAX_PAGES / 64];
static uint64_t page_base = 0;
static uint64_t total_pages = 0;

static inline void set_bit(uint64_t idx) { bitmap[idx >> 6] |= (1ULL << (idx & 63)); }
static inline void clear_bit(uint64_t idx) { bitmap[idx >> 6] &= ~(1ULL << (idx & 63)); }
static inline int test_bit(uint64_t idx) { return (bitmap[idx >> 6] >> (idx & 63)) & 1; }

static inline uint64_t irq_save_disable(void) {
    uint64_t rflags;
    __asm__ volatile ("pushfq; pop %0; cli" : "=r"(rflags) : : "memory");
    return rflags;
}

static inline void irq_restore(uint64_t rflags) {
    __asm__ volatile ("push %0; popfq" : : "r"(rflags) : "memory", "cc");
}

void pmm_init(uint64_t start, uint64_t end) {
    // Align to pages
    start = (start + PMM_PAGE_SIZE - 1) & ~(PMM_PAGE_SIZE - 1);
    end   = end & ~(PMM_PAGE_SIZE - 1);
    if (end <= start) return;

    page_base = start;
    total_pages = (end - start) / PMM_PAGE_SIZE;
    if (total_pages > MAX_PAGES) total_pages = MAX_PAGES;

    // Mark all free initially
    for (uint64_t i = 0; i < (total_pages + 63) / 64; i++) {
        bitmap[i] = 0;
    }
}

void *pmm_alloc_pages(uint64_t count) {
    if (count == 0 || count > total_pages) return 0;

    // Single-core kernel: protect allocator bitmap from preemptive interleaving.
    uint64_t irq_flags = irq_save_disable();

    for (uint64_t start = 0; start + count <= total_pages; start++) {
        int all_free = 1;
        for (uint64_t i = 0; i < count; i++) {
            if (test_bit(start + i)) {
                all_free = 0;
                break;
            }
        }
        if (!all_free) continue;

        for (uint64_t i = 0; i < count; i++) {
            set_bit(start + i);
        }
        void *ret = (void *)(page_base + start * PMM_PAGE_SIZE);
        irq_restore(irq_flags);
        return ret;
    }

    irq_restore(irq_flags);
    return 0;
}

void *pmm_alloc_page(void) {
    return pmm_alloc_pages(1);
}

void pmm_free_pages(void *base, uint64_t count) {
    if (!base || count == 0) return;

    uint64_t irq_flags = irq_save_disable();

    uint64_t addr = (uint64_t)base;
    if (addr < page_base) {
        irq_restore(irq_flags);
        return;
    }
    uint64_t idx = (addr - page_base) / PMM_PAGE_SIZE;
    if (idx >= total_pages) {
        irq_restore(irq_flags);
        return;
    }

    for (uint64_t i = 0; i < count && (idx + i) < total_pages; i++) {
        clear_bit(idx + i);
    }
    irq_restore(irq_flags);
}

void pmm_free_page(void *page) {
    pmm_free_pages(page, 1);
}

#include "window.h"
#include "paging.h"
#include "pmm.h"

static struct win_entry windows[MAX_WINDOWS];

static void window_release_slot(int slot) {
    if (slot < 0 || slot >= MAX_WINDOWS) return;

    struct win_entry *win = &windows[slot];
    uint64_t owner_vaddr = WIN_APP_VA_BASE + (uint64_t)slot * WIN_SLOT_SIZE;

    for (int i = 0; i < win->page_count; i++) {
        uint64_t pa = 0;
        int unmapped = 0;

        if (win->owner_cr3) {
            if (paging_unmap_page((uint64_t *)win->owner_cr3,
                                  owner_vaddr + (uint64_t)i * 0x1000,
                                  &pa) == 0) {
                unmapped = 1;
            }
        }

        if (unmapped && pa) {
            pmm_free_page((void *)pa);
        } else if (win->phys_pages[i]) {
            pmm_free_page((void *)win->phys_pages[i]);
        }
        win->phys_pages[i] = 0;
    }

    win->active = 0;
    win->owner_pid = 0;
    win->owner_cr3 = 0;
    win->width = 0;
    win->height = 0;
    win->flags = 0;
    win->dirty = 0;
    win->page_count = 0;
    win->ev_head = 0;
    win->ev_tail = 0;
    win->ev_count = 0;
}

void window_init(void) {
    for (int i = 0; i < MAX_WINDOWS; i++) {
        windows[i].active = 0;
        windows[i].owner_pid = 0;
        windows[i].owner_cr3 = 0;
        windows[i].width = 0;
        windows[i].height = 0;
        windows[i].flags = 0;
        windows[i].dirty = 0;
        windows[i].page_count = 0;
        windows[i].ev_head = 0;
        windows[i].ev_tail = 0;
        windows[i].ev_count = 0;
    }
}

int window_create(int pid, uint64_t cr3, int flags, int w, int h) {
    if (w <= 0 || h <= 0 || w > 1024 || h > 768) return -1;

    // Find free slot
    int slot = -1;
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (!windows[i].active) {
            slot = i;
            break;
        }
    }
    if (slot < 0) return -1;

    uint64_t bytes = (uint64_t)w * (uint64_t)h * 4;
    int pages = (int)((bytes + 0xFFF) / 0x1000);
    if (pages > WIN_MAX_PAGES) return -1;

    struct win_entry *win = &windows[slot];

    // Allocate and zero physical pages
    for (int i = 0; i < pages; i++) {
        void *page = pmm_alloc_page();
        if (!page) {
            // Free already allocated pages
            for (int j = 0; j < i; j++) {
                pmm_free_page((void *)win->phys_pages[j]);
            }
            return -1;
        }
        // Zero the page
        uint64_t *pz = (uint64_t *)page;
        for (int q = 0; q < 512; q++) pz[q] = 0;
        win->phys_pages[i] = (uint64_t)page;
    }

    // Map into owner's address space at WIN_APP_VA_BASE + slot * 4MB
    uint64_t vaddr = WIN_APP_VA_BASE + (uint64_t)slot * WIN_SLOT_SIZE;
    for (int i = 0; i < pages; i++) {
        if (paging_map_user_page((uint64_t *)cr3,
                                  vaddr + (uint64_t)i * 0x1000,
                                  win->phys_pages[i],
                                  PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER) < 0) {
            // Cleanup on failure
            for (int j = 0; j < pages; j++) {
                pmm_free_page((void *)win->phys_pages[j]);
            }
            return -1;
        }
    }

    win->active = 1;
    win->owner_pid = pid;
    win->owner_cr3 = cr3;
    win->width = w;
    win->height = h;
    win->flags = flags;
    win->dirty = 0;
    win->page_count = pages;
    win->ev_head = 0;
    win->ev_tail = 0;
    win->ev_count = 0;

    return slot;
}

int window_present(int handle, int pid, int x, int y, int w, int h) {
    if (handle < 0 || handle >= MAX_WINDOWS) return -1;
    struct win_entry *win = &windows[handle];
    if (!win->active || win->owner_pid != pid) return -1;

    (void)x; (void)y; (void)w; (void)h;
    win->dirty = 1;
    return 0;
}

int window_close(int handle, int pid) {
    if (handle < 0 || handle >= MAX_WINDOWS) return -1;
    struct win_entry *win = &windows[handle];
    if (!win->active || win->owner_pid != pid) return -1;

    window_release_slot(handle);
    return 0;
}

int window_poll_event(int handle, int pid, struct user_input_event *out) {
    if (handle < 0 || handle >= MAX_WINDOWS) return 0;
    struct win_entry *win = &windows[handle];
    if (!win->active || win->owner_pid != pid) return 0;
    if (win->ev_count == 0) return 0;

    *out = win->events[win->ev_tail];
    win->ev_tail = (win->ev_tail + 1) % WIN_EVENT_SLOTS;
    win->ev_count--;
    return 1;
}

int window_get_info(int slot, struct user_win_info *out) {
    if (slot < 0 || slot >= MAX_WINDOWS || !out) return 0;
    struct win_entry *win = &windows[slot];
    if (!win->active) {
        out->active = 0;
        return 0;
    }

    out->active = 1;
    out->owner_pid = win->owner_pid;
    out->width = win->width;
    out->height = win->height;
    out->dirty = win->dirty;
    return 1;
}

uint64_t window_map_for_compositor(int handle, uint64_t compositor_cr3) {
    if (handle < 0 || handle >= MAX_WINDOWS) return 0;
    struct win_entry *win = &windows[handle];
    if (!win->active) return 0;

    uint64_t vaddr = WIN_COMPOSITOR_VA_BASE + (uint64_t)handle * WIN_SLOT_SIZE;

    // Map same physical pages read-only into compositor's address space
    for (int i = 0; i < win->page_count; i++) {
        if (paging_map_user_shared_page((uint64_t *)compositor_cr3,
                                        vaddr + (uint64_t)i * 0x1000,
                                        win->phys_pages[i],
                                        PAGE_PRESENT | PAGE_USER) < 0) {
            return 0;
        }
    }

    return vaddr;
}

int window_send_event(int handle, const struct user_input_event *ev) {
    if (handle < 0 || handle >= MAX_WINDOWS || !ev) return 0;
    struct win_entry *win = &windows[handle];
    if (!win->active) return 0;

    if (win->ev_count >= WIN_EVENT_SLOTS) {
        // Drop oldest event
        win->ev_tail = (win->ev_tail + 1) % WIN_EVENT_SLOTS;
        win->ev_count--;
    }

    win->events[win->ev_head] = *ev;
    win->ev_head = (win->ev_head + 1) % WIN_EVENT_SLOTS;
    win->ev_count++;
    return 1;
}

void window_cleanup_pid(int pid) {
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (windows[i].active && windows[i].owner_pid == pid) {
            window_release_slot(i);
        }
    }
}

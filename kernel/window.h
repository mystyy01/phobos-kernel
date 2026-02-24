#ifndef WINDOW_H
#define WINDOW_H

#include <stdint.h>
#include "syscall.h"

#define MAX_WINDOWS 16
#define WIN_MAX_PAGES 1024
#define WIN_EVENT_SLOTS 32

// Virtual address bases for window buffer mapping
#define WIN_COMPOSITOR_VA_BASE 0x58000000ULL  // read-only compositor mappings
#define WIN_APP_VA_BASE        0x60000000ULL  // read-write app buffers
#define WIN_SLOT_SIZE          0x400000ULL    // 4MB per slot

// Info struct passed to userspace via SYS_WIN_INFO
struct user_win_info {
    int active;
    int owner_pid;
    int width;
    int height;
    int dirty;
};

struct win_entry {
    int active;
    int owner_pid;
    int width;
    int height;
    int flags;
    int dirty;
    uint64_t owner_cr3;
    uint64_t phys_pages[WIN_MAX_PAGES];
    int page_count;
    struct user_input_event events[WIN_EVENT_SLOTS];
    int ev_head;
    int ev_tail;
    int ev_count;
};

void window_init(void);
int window_create(int pid, uint64_t cr3, int flags, int w, int h);
int window_present(int handle, int pid, int x, int y, int w, int h);
int window_close(int handle, int pid);
int window_poll_event(int handle, int pid, struct user_input_event *out);
int window_get_info(int slot, struct user_win_info *out);
uint64_t window_map_for_compositor(int handle, uint64_t compositor_cr3);
int window_send_event(int handle, const struct user_input_event *ev);
void window_cleanup_pid(int pid);

#endif

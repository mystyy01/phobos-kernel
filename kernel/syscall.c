#include "syscall.h"
#include "fs/fat32.h"
#include "fs/vfs.h"
#include "elf_loader.h"
#include "sched.h"
#include "paging.h"
#include "pmm.h"
#include "isr.h"
#include "tty.h"
#include "console.h"
#include "drivers/framebuffer.h"
#include "drivers/keyboard.h"
#include "drivers/mouse.h"

extern void sched_deliver_signals(struct task *t);

// ============================================================================
// Console I/O
// ============================================================================

static int console_fd_write(const char *buf, int count) {
    return console_write(buf, count);
}

// ============================================================================
// MSR helpers
// ============================================================================

static inline void wrmsr(uint32_t msr, uint64_t value) {
    uint32_t low = value & 0xFFFFFFFF;
    uint32_t high = value >> 32;
    __asm__ volatile ("wrmsr" : : "c"(msr), "a"(low), "d"(high));
}

static inline uint64_t rdmsr(uint32_t msr) {
    uint32_t low, high;
    __asm__ volatile ("rdmsr" : "=a"(low), "=d"(high) : "c"(msr));
    return ((uint64_t)high << 32) | low;
}

// ============================================================================
// String helpers
// ============================================================================

static int str_len(const char *s) {
    int len = 0;
    while (s[len]) len++;
    return len;
}

static void str_copy(char *dst, const char *src, int max) {
    int i = 0;
    while (src[i] && i < max - 1) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

static void build_path(const char *path, char *out) {
    struct task *t = sched_current();
    if (path[0] == '/') {
        str_copy(out, path, VFS_MAX_PATH);
    } else {
        int cwd_len = str_len(t->cwd);
        str_copy(out, t->cwd, VFS_MAX_PATH);

        if (cwd_len > 0 && t->cwd[cwd_len-1] != '/') {
            out[cwd_len] = '/';
            out[cwd_len + 1] = '\0';
            cwd_len++;
        }

        int i = 0;
        while (path[i] && cwd_len + i < VFS_MAX_PATH - 1) {
            out[cwd_len + i] = path[i];
            i++;
        }
        out[cwd_len + i] = '\0';
    }
}

// ============================================================================
// Syscall initialization
// ============================================================================

extern void syscall_entry(void);

void syscall_init(void) {

    uint64_t efer = rdmsr(MSR_EFER);
    efer |= EFER_SCE;
    wrmsr(MSR_EFER, efer);

    // STAR: kernel CS = 0x08, user CS base = 0x10 (so SYSRET gives CS=0x20|3, SS=0x18|3)
    uint64_t star = ((uint64_t)0x0008 << 32) | ((uint64_t)0x0010 << 48);
    wrmsr(MSR_STAR, star);

    wrmsr(MSR_LSTAR, (uint64_t)syscall_entry);
    wrmsr(MSR_FMASK, 0x200);
}

// ============================================================================
// Syscall handler
// ============================================================================

// User context saved by syscall_entry.asm — used by fork() in sched.c
extern uint64_t user_ctx_rsp;
extern uint64_t user_ctx_rip;
extern uint64_t user_ctx_rflags;

uint64_t syscall_handler(uint64_t num, uint64_t arg1, uint64_t arg2,
                         uint64_t arg3, uint64_t arg4, uint64_t arg5) {
    (void)arg4; (void)arg5;

    switch (num) {

        case SYS_EXIT: {
            int exit_code = (int)arg1;
            sched_exit(exit_code);
            __builtin_unreachable();
            return 0;
        }

        case SYS_READ: {
            int fd = (int)arg1;
            char *buf = (char *)arg2;
            int count = (int)arg3;

            struct task *t = sched_current();
            struct fd_entry *entry = task_fd_get(t, fd);
            if (!entry) return -1;

            if (entry->type == FD_CONSOLE) {
                return 0;  
            }

            if (entry->type == FD_FILE && entry->node) {
                int bytes = vfs_read(entry->node, entry->offset, count, (uint8_t *)buf);
                if (bytes > 0) {
                    entry->offset += bytes;
                }
                return bytes;
            }
            if (entry->type == FD_PIPE){
                struct pipe *p = entry->pipe;
                if (p->count == 0) return 0;
                int to_read = p->count;
                if (to_read > count) to_read = count;
                for (int i = 0; i < to_read; i++){
                    buf[i] = p->buffer[p->read_pos];
                    p->read_pos = (p->read_pos + 1) % PIPE_BUF_SIZE;
                    p->count--;
                }
                return to_read;
            }

            return -1;
        }

        case SYS_WRITE: {
            int fd = (int)arg1;
            const char *buf = (const char *)arg2;
            int count = (int)arg3;
            struct task *t = sched_current();
            struct fd_entry *entry = task_fd_get(t, fd);
            if (!entry) return -1;

            if (entry->type == FD_CONSOLE) {
                return console_fd_write(buf, count);
            }
            if (entry->type == FD_PIPE){
                struct pipe *p = entry->pipe;
                if (p->count >= PIPE_BUF_SIZE) return 0;
                int free_space = PIPE_BUF_SIZE - p->count;
                int to_write = count;
                if (to_write > free_space) to_write = free_space;
                for (int i = 0; i < to_write; i++){
                    p->buffer[p->write_pos] = buf[i];
                    p->write_pos = (p->write_pos + 1) % PIPE_BUF_SIZE;
                    p->count++;
                }
                return to_write;
            }
            if (entry->type == FD_FILE && entry->node) {
                int bytes = vfs_write(entry->node, entry->offset, count, (const uint8_t *)buf);
                if (bytes > 0) {
                    entry->offset += bytes;
                }
                return bytes;
            }
            return -1;
        }

        case SYS_OPEN: {
            const char *path = (const char *)arg1;
            int flags = (int)arg2;

            char full_path[VFS_MAX_PATH];
            build_path(path, full_path);

            struct vfs_node *node = vfs_resolve_path(full_path);
            if (!node && (flags & O_CREAT)) {
                fat32_touch_path(full_path);
                node = vfs_resolve_path(full_path);
            }
            if (!node) return -1;

            if ((flags & O_TRUNC) && (node->flags & VFS_FILE)) {
                fat32_truncate(node, 0);
            }

            struct task *t = sched_current();
            int fd = task_fd_alloc(t);

            if (fd < 0) return -1;

            t->fd_table[fd].node = node;
            t->fd_table[fd].offset = 0;
            t->fd_table[fd].flags = flags;

            if (node->flags & VFS_DIRECTORY) {
                t->fd_table[fd].type = FD_DIR;
            } else {
                t->fd_table[fd].type = FD_FILE;
            }

            if ((flags & O_APPEND) && (t->fd_table[fd].type == FD_FILE)) {
                t->fd_table[fd].offset = node->size;
            }

            return fd;
        }

        case SYS_CLOSE: {
            int fd = (int)arg1;
            struct task *t = sched_current();
            if (fd < 0) return -1;
            struct fd_entry *entry = task_fd_get(t, fd);
            if (!entry || fd < 3) return -1;
            if (entry->type == FD_FILE && entry->node) {
                fat32_flush_size(entry->node);
            }
            if (entry->type == FD_PIPE && entry->pipe) {
                if (entry->flags == O_RDONLY) {
                    entry->pipe->read_open = 0;
                } else if (entry->flags == O_WRONLY) {
                    entry->pipe->write_open = 0;
                }
            }
            task_fd_free(t, fd);
            return 0;
        }

        case SYS_STAT: {
            const char *path = (const char *)arg1;
            struct stat *buf = (struct stat *)arg2;

            char full_path[VFS_MAX_PATH];
            build_path(path, full_path);

            struct vfs_node *node = vfs_resolve_path(full_path);
            if (!node) return -1;

            buf->st_size = node->size;
            buf->st_ino = node->inode;
            buf->st_mode = (node->flags & VFS_DIRECTORY) ? S_IFDIR : S_IFREG;

            return 0;
        }

        case SYS_FSTAT: {
            int fd = (int)arg1;
            if (fd < 0) return -1;
            struct stat *buf = (struct stat *)arg2;
            struct task *t = sched_current();
            struct fd_entry *entry = task_fd_get(t, fd);
            if (!entry || !entry->node) return -1;

            buf->st_size = entry->node->size;
            buf->st_ino = entry->node->inode;
            buf->st_mode = (entry->node->flags & VFS_DIRECTORY) ? S_IFDIR : S_IFREG;

            return 0;
        }

        case SYS_MKDIR: {
            const char *path = (const char *)arg1;

            char full_path[VFS_MAX_PATH];
            build_path(path, full_path);

            struct vfs_node *result = ensure_path_exists(full_path);
            return result ? 0 : -1;
        }

        case SYS_RMDIR: {
            const char *path = (const char *)arg1;
            char full_path[VFS_MAX_PATH];
            build_path(path, full_path);
            int res = fat32_rmdir_path(full_path);
            return (res == 0) ? 0 : -1;
        }

        case SYS_UNLINK: {
            const char *path = (const char *)arg1;
            char full_path[VFS_MAX_PATH];
            build_path(path, full_path);
            int res = fat32_rm_path(full_path);
            return (res == 0) ? 0 : -1;
        }

        case SYS_READDIR: {
            int fd = (int)arg1;
            if (fd < 0) return -1;
            struct user_dirent *buf = (struct user_dirent *)arg2;
            int index = (int)arg3;

            struct task *t = sched_current();

            struct fd_entry *entry = task_fd_get(t, fd);
            if (!entry || entry->type != FD_DIR || !entry->node) return -1;

            struct dirent *dent = vfs_readdir(entry->node, index);
            if (!dent) return -1;

            str_copy(buf->name, dent->name, 256);

            struct vfs_node *child = vfs_finddir(entry->node, dent->name);
            buf->type = (child && (child->flags & VFS_DIRECTORY)) ? 1 : 0;

            return 0;
        }

        case SYS_CHDIR: {
            struct task *t = sched_current();
            const char *path = (const char *)arg1;

            char full_path[VFS_MAX_PATH];
            build_path(path, full_path);

            struct vfs_node *node = vfs_resolve_path(full_path);
            if (!node) return -1;
            if (!(node->flags & VFS_DIRECTORY)) return -1;

            str_copy(t->cwd, full_path, VFS_MAX_PATH);
            return 0;
        }

        case SYS_GETCWD: {
            struct task *t = sched_current();
            char *buf = (char *)arg1;
            int size = (int)arg2;

            int len = str_len(t->cwd);
            if (len >= size) return -1;

            str_copy(buf, t->cwd, size);
            return len;
        }

        case SYS_RENAME:
            // rename(old, new)
        {
            const char *oldp = (const char *)arg1;
            const char *newp = (const char *)arg2;
            char full_old[VFS_MAX_PATH];
            char full_new[VFS_MAX_PATH];
            build_path(oldp, full_old);
            build_path(newp, full_new);
            int res = fat32_mv_path(full_old, full_new);
            return (res == 0) ? 0 : -1;
        }

        case SYS_TRUNCATE: {
            const char *path = (const char *)arg1;
            int size = (int)arg2;
            char full_path[VFS_MAX_PATH];
            build_path(path, full_path);
            struct vfs_node *node = vfs_resolve_path(full_path);
            if (!node) return -1;
            return (fat32_truncate(node, size) == 0) ? 0 : -1;
        }

        case SYS_CREATE: {
            const char *path = (const char *)arg1;
            char full_path[VFS_MAX_PATH];
            build_path(path, full_path);
            int res = fat32_touch_path(full_path);
            return (res == 0) ? 0 : -1;
        }

        case SYS_SEEK: {
            struct task *t = sched_current();
            int fd = (int)arg1;
            struct fd_entry *entry = task_fd_get(t, fd);
            if (!entry || entry->type == FD_CONSOLE || entry->type == FD_DIR) return -1;
            int offset = (int)arg2;
            int whence = (int)arg3;
            int new_offset = 0;
            if (whence == SEEK_SET) new_offset = offset;
            if (whence == SEEK_CUR) new_offset = entry->offset + offset;
            if (whence == SEEK_END) new_offset = entry->node->size + offset;
            if (new_offset < 0) return -1;
            entry->offset = new_offset;
            return new_offset;
        }

        case SYS_YIELD: {
            sched_yield();
            return 0;
        }
        case SYS_PIPE: {
            int *fds = (int *)arg1;

            struct task *t = sched_current();
            struct pipe *pipe = pipe_alloc();
            if (!pipe) return -1;
            int read_fd = task_fd_alloc(t);
            if (read_fd < 0) return -1;

            int write_fd = task_fd_alloc(t);
            if (write_fd < 0){
                task_fd_free(t, read_fd);
                return -1;
            }
            // setup read_fd 
            t->fd_table[read_fd].type = FD_PIPE;
            t->fd_table[read_fd].pipe = pipe;
            t->fd_table[read_fd].flags = O_RDONLY;

            // setup write_fd
            t->fd_table[write_fd].type = FD_PIPE;
            t->fd_table[write_fd].pipe = pipe;
            t->fd_table[write_fd].flags = O_WRONLY;

            fds[0] = read_fd;
            fds[1] = write_fd;

            return 0;
        }
        case SYS_DUP2: {
            int oldfd = (int)arg1;
            int newfd = (int)arg2;
            struct task *t = sched_current();
            struct fd_entry *old = task_fd_get(t, oldfd);
            if (!old) return -1;
            if (newfd < 0 || newfd >= MAX_FDS) return -1;
            if (t->fd_table[newfd].type != FD_UNUSED) task_fd_free(t, newfd); 
            t->fd_table[newfd] = t->fd_table[oldfd];
            return newfd;
        }

        case SYS_FORK: {
            return (uint64_t)sched_fork();
        }

        case SYS_EXEC: {
            const char *path = (const char *)arg1;
            // For now exec is not implemented as a syscall
            // (shell uses kernel-level sched_spawn instead)
            (void)path;
            return -1;
        }

        case SYS_WAITPID: {
            int pid = (int)arg1;
            return sched_waitpid(pid);
        }

        case SYS_GETPID: {
            struct task *t = sched_current();
            return t ? (uint64_t)t->id : 0;
        }
        case SYS_KILL: {
            int pid = (int)arg1;
            int sig = (int)arg2;
            struct task *task = sched_get_task(pid);
            if (!task) return -1;
            if (sig < 0) return -1;
            if (sig == SIGKILL){
                task->state = TASK_STATE_ZOMBIE;
                task->exit_code = -1;
                sched_wake_waiters(pid);
                return 0;
            }
            task->pending_signals |= (1ULL << sig);
            return 0;
        }
        case SYS_SIGNAL: {
            int sig = (int)arg1;
            void *handler = (void *)arg2;

            struct task *t = sched_current();
            if (sig < 1 || sig > 31) return -1;

            uint64_t old = t->signal_handlers[sig];
            t->signal_handlers[sig] = (uint64_t)handler;

            return old;
        }

        case SYS_SETPGID: {
            int pid = (int)arg1;
            int pgid = (int)arg2;

            // If pid is 0, use current process
            if (pid == 0) pid = (int)sched_current()->id;
            // If pgid is 0, set pgid = pid
            if (pgid == 0) pgid = pid;

            struct task *t = sched_get_task(pid);
            if (!t) return -1;

            t->pgid = pgid;
            return 0;
        }

        case SYS_TCSETPGRP: {
            int pgid = (int)arg1;
            tty_set_foreground_pgid(pgid);
            return 0;
        }

        case SYS_TCGETPGRP: {
            return tty_get_foreground_pgid();
        }

        // =========================================================================
        // Rendering syscalls
        // =========================================================================

        case SYS_FB_INFO: {
            struct user_fb_info *out = (struct user_fb_info *)arg1;
            if (!out) return -1;

            out->width = (uint32_t)fb_width();
            out->height = (uint32_t)fb_height();
            out->bpp = (uint32_t)fb_bpp();

            // TODO(HUMAN): Replace with real hardware pitch if you expose it.
            out->pitch = out->width * (out->bpp / 8);
            return 0;
        }

        case SYS_FB_PUTPIXEL: {
            int x = (int)arg1;
            int y = (int)arg2;
            uint32_t colour = (uint32_t)arg3;

            fb_putpixel(x, y, colour);

            // TODO(HUMAN): For speed, replace this with a backbuffer+present path.
            return 0;
        }

        case SYS_INPUT_POLL: {
            struct user_input_event *out = (struct user_input_event *)arg1;
            if (!out) return -1;

            struct key_event ev;
            if (!keyboard_poll_event(&ev)) {
                struct mouse_event mev;
                if (!mouse_poll_event(&mev)) {
                    return 0; // no event available
                }

                out->type = (uint8_t)((mev.type == MOUSE_EVENT_BUTTON)
                    ? INPUT_EVENT_MOUSE_BUTTON
                    : INPUT_EVENT_MOUSE_MOVE);
                out->key = 0;
                out->modifiers = 0;
                out->pressed = (uint8_t)mev.pressed;
                out->scancode = (uint8_t)mev.button;
                out->mouse_buttons = (uint8_t)mev.buttons;
                out->mouse_x = (int16_t)mev.x;
                out->mouse_y = (int16_t)mev.y;
                return 1;
            }

            out->type = INPUT_EVENT_KEYBOARD;
            out->key = ev.key;
            out->modifiers = ev.modifiers;
            out->pressed = ev.pressed;
            out->scancode = ev.scancode;
            out->mouse_buttons = mouse_get_buttons();
            out->mouse_x = (int16_t)mouse_get_x();
            out->mouse_y = (int16_t)mouse_get_y();
            return 1; // event written
        }

        case SYS_TICKS: {
            extern volatile uint64_t system_ticks;
            return system_ticks;
        }

        case SYS_FB_MAP: {
            struct task *t = sched_current();
            if (!t) return 0;

            uint64_t w = (uint64_t)fb_width();
            uint64_t h = (uint64_t)fb_height();
            uint64_t b = (uint64_t)fb_bpp();
            uint64_t bytes_pp = b ? b / 8 : 4;
            uint64_t size = w * h * bytes_pp;
            if (size == 0) return 0;

            uint64_t pages = (size + 0xFFF) / 0x1000;
            uint64_t vaddr = 0x2000000ULL; // Fixed backbuffer base

            for (uint64_t i = 0; i < pages; i++) {
                void *page = pmm_alloc_page();
                if (!page) return 0;
                paging_map_user_page((uint64_t *)t->cr3,
                                     vaddr + i * 0x1000,
                                     (uint64_t)page,
                                     PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER);
            }

            // Zero through the now-mapped virtual address (safe with any CR3)
            uint64_t *vp = (uint64_t *)vaddr;
            uint64_t qwords = size / 8;
            for (uint64_t i = 0; i < qwords; i++) vp[i] = 0;

            return vaddr;
        }

        case SYS_FB_PRESENT: {
            const void *src = (const void *)arg1;
            if (!src) return -1;

            uint64_t w = (uint64_t)fb_width();
            uint64_t h = (uint64_t)fb_height();
            uint64_t b = (uint64_t)fb_bpp();
            uint64_t bytes_pp = b ? b / 8 : 4;
            uint64_t size = w * h * bytes_pp;

            fb_present_buffer(src, size);
            return 0;
        }

        default:
            return -1;
    }
    struct task *t = sched_current();
    if (t) sched_deliver_signals(t);
}

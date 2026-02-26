// PHOBOS Userspace Syscall Library
// Include this in your apps to use syscalls

#ifndef LIBSYS_H
#define LIBSYS_H

// ============================================================================
// Syscall Numbers (must match kernel/syscall.h)
// ============================================================================

#define SYS_EXIT      0   // exit(int code)
#define SYS_READ      1   // read(int fd, char *buf, int count) -> bytes read
#define SYS_WRITE     2   // write(int fd, char *buf, int count) -> bytes written
#define SYS_OPEN      3   // open(char *path, int flags) -> fd
#define SYS_CLOSE     4   // close(int fd) -> 0 or -1
#define SYS_STAT      5   // stat(char *path, struct stat *buf) -> 0 or -1
#define SYS_FSTAT     6   // fstat(int fd, struct stat *buf) -> 0 or -1
#define SYS_MKDIR     7   // mkdir(char *path) -> 0 or -1
#define SYS_RMDIR     8   // rmdir(char *path) -> 0 or -1
#define SYS_UNLINK    9   // unlink(char *path) -> 0 or -1
#define SYS_READDIR   10  // readdir(int fd, struct dirent *buf, int index) -> 0 or -1
#define SYS_CHDIR     11  // chdir(char *path) -> 0 or -1
#define SYS_GETCWD    12  // getcwd(char *buf, int size) -> buf or NULL
#define SYS_RENAME    13  // rename(char *old, char *new) -> 0 or -1
#define SYS_TRUNCATE  14  // truncate(char *path, int size) -> 0 or -1
#define SYS_CREATE    15  // create(char *path) -> fd or -1 (create new file)
#define SYS_SEEK      16  // seek(int fd, int offset, int whence) -> new offset or -1
#define SYS_YIELD     17  // yield() -> 0
#define SYS_PIPE      18  // pipe(int fds[2]) -> 0 or -1
#define SYS_DUP2      19  // dup2(int oldfd, int newfd) -> newfd or -1
#define SYS_FORK      20  // fork() -> child pid to parent, 0 to child
#define SYS_EXEC      21  // exec(char *path, char **argv) -> -1 on error
#define SYS_WAITPID   22  // waitpid(int pid) -> exit code or -1
#define SYS_GETPID    23  // getpid() -> current pid
#define SYS_KILL      24  // kill(int pid, int sig) -> 0 or -1
#define SYS_SIGNAL    25  // signal(int sig, void *handler) -> old handler
#define SYS_SETPGID   26  // setpgid(int pid, int pgid) -> 0 or -1
#define SYS_TCSETPGRP 27  // tcsetpgrp(int pgid) -> 0 or -1
#define SYS_TCGETPGRP 28  // tcgetpgrp() -> pgid
#define SYS_FB_INFO   29  // fb_info(struct user_fb_info *out) -> 0 or -1
#define SYS_FB_PUTPIXEL 30 // fb_putpixel(int x, int y, unsigned int colour) -> 0
#define SYS_INPUT_POLL 31 // input_poll(struct user_input_event *out) -> 1, 0, or -1
#define SYS_TICKS     32  // ticks() -> current tick count
#define SYS_FB_MAP    33  // fb_map() -> vaddr of user backbuffer
#define SYS_FB_PRESENT 34 // fb_present(void *buf) -> 0
#define SYS_FB_PRESENT_RECT 35 // fb_present_rect(void *buf, int x, int y, int w, int h) -> 0

// Window management syscalls
#define SYS_WIN_CREATE  36 // win_create(flags, w, h) -> handle (0-15) or -1
#define SYS_WIN_PRESENT 37 // win_present(handle, x, y, w, h) -> 0 or -1
#define SYS_WIN_CLOSE   38 // win_close(handle) -> 0 or -1
#define SYS_WIN_POLL    39 // win_poll(handle, &event) -> 1 or 0
#define SYS_WIN_INFO    40 // win_info(slot, &info) -> 1 or 0
#define SYS_WIN_MAP     41 // win_map(handle) -> vaddr or 0
#define SYS_WIN_SEND    42 // win_send(handle, &event) -> 1 or 0

// signal numbers
#define SIGKILL     9
#define SIGTERM     15
#define SIGINT      2
#define SIGTSTP     20
#define SIGCHLD     17
#define SIGCONT     18

// ============================================================================
// Open Flags
// ============================================================================

#define O_RDONLY    0x0000
#define O_WRONLY    0x0001
#define O_RDWR      0x0002
#define O_CREAT     0x0100
#define O_TRUNC     0x0200
#define O_APPEND    0x0400

// ============================================================================
// Seek Whence
// ============================================================================

#define SEEK_SET    0
#define SEEK_CUR    1
#define SEEK_END    2

// ============================================================================
// Standard File Descriptors
// ============================================================================

#define STDIN       0
#define STDOUT      1
#define STDERR      2

// ============================================================================
// Stat Structure
// ============================================================================

struct stat {
    unsigned int st_size;      // File size in bytes
    unsigned int st_mode;      // File type and permissions
    unsigned int st_ino;       // Inode number
};

#define S_IFREG     0x8000  // Regular file
#define S_IFDIR     0x4000  // Directory

// ============================================================================
// Directory Entry (for readdir)
// ============================================================================

struct dirent {
    char name[256];
    unsigned int type;  // 0 = file, 1 = directory
};

// ============================================================================
// GUI ABI structures
// ============================================================================

struct user_fb_info {
    unsigned int width;
    unsigned int height;
    unsigned int bpp;
    unsigned int pitch;
};

struct user_input_event {
    unsigned char type;
    unsigned char key;
    unsigned char modifiers;
    unsigned char pressed;
    unsigned char scancode;
    unsigned char mouse_buttons;
    short mouse_x;
    short mouse_y;
};

#define INPUT_EVENT_KEYBOARD     1
#define INPUT_EVENT_MOUSE_MOVE   2
#define INPUT_EVENT_MOUSE_BUTTON 3

#define MOD_SHIFT 0x01
#define MOD_CTRL  0x02
#define MOD_ALT   0x04
#define MOD_SUPER 0x08

// ============================================================================
// Syscall Wrappers
// ============================================================================

static inline long syscall0(long num) {
    long result;
    __asm__ volatile (
        "syscall"
        : "=a"(result)
        : "a"(num)
        : "rcx", "r11", "rdi", "rsi", "rdx", "r8", "r9", "r10", "memory"
    );
    return result;
}

static inline long syscall1(long num, long arg1) {
    long result;
    long _rdi = arg1;
    __asm__ volatile (
        "syscall"
        : "=a"(result), "+D"(_rdi)
        : "a"(num)
        : "rcx", "r11", "rsi", "rdx", "r8", "r9", "r10", "memory"
    );
    return result;
}

static inline long syscall2(long num, long arg1, long arg2) {
    long result;
    long _rdi = arg1, _rsi = arg2;
    __asm__ volatile (
        "syscall"
        : "=a"(result), "+D"(_rdi), "+S"(_rsi)
        : "a"(num)
        : "rcx", "r11", "rdx", "r8", "r9", "r10", "memory"
    );
    return result;
}

static inline long syscall3(long num, long arg1, long arg2, long arg3) {
    long result;
    long _rdi = arg1, _rsi = arg2, _rdx = arg3;
    __asm__ volatile (
        "syscall"
        : "=a"(result), "+D"(_rdi), "+S"(_rsi), "+d"(_rdx)
        : "a"(num)
        : "rcx", "r11", "r8", "r9", "r10", "memory"
    );
    return result;
}

static inline long syscall4(long num, long arg1, long arg2, long arg3, long arg4) {
    long result;
    long _rdi = arg1, _rsi = arg2, _rdx = arg3;
    register long r10 __asm__("r10") = arg4;
    __asm__ volatile (
        "syscall"
        : "=a"(result), "+D"(_rdi), "+S"(_rsi), "+d"(_rdx), "+r"(r10)
        : "a"(num)
        : "rcx", "r11", "r8", "r9", "memory"
    );
    return result;
}

static inline long syscall5(long num, long arg1, long arg2, long arg3, long arg4, long arg5) {
    long result;
    long _rdi = arg1, _rsi = arg2, _rdx = arg3;
    register long r10 __asm__("r10") = arg4;
    register long r8 __asm__("r8") = arg5;
    __asm__ volatile (
        "syscall"
        : "=a"(result), "+D"(_rdi), "+S"(_rsi), "+d"(_rdx), "+r"(r10), "+r"(r8)
        : "a"(num)
        : "rcx", "r11", "r9", "memory"
    );
    return result;
}


// ============================================================================
// Convenience Functions
// ============================================================================

static inline void exit(int code) {
    syscall1(SYS_EXIT, code);
    while (1) __asm__ volatile ("hlt");
}

static inline int strlen(const char *s) {
    int len = 0;
    while (s[len]) len++;
    return len;
}

static inline void print(const char *s) {
    syscall3(SYS_WRITE, STDOUT, (long)s, strlen(s));
}

static inline void eprint(const char *s) {
    syscall3(SYS_WRITE, STDERR, (long)s, strlen(s));
}

static inline int open(const char *path, int flags) {
    return (int)syscall2(SYS_OPEN, (long)path, flags);
}

static inline int close(int fd) {
    return (int)syscall1(SYS_CLOSE, fd);
}

static inline int read(int fd, char *buf, int count) {
    return (int)syscall3(SYS_READ, fd, (long)buf, count);
}
static inline int write(int fd, const char *buf, int count) {
    return (int)syscall3(SYS_WRITE, fd, (long)buf, count);
}

static inline int stat(const char *path, struct stat *buf) {
    return (int)syscall2(SYS_STAT, (long)path, (long)buf);
}

static inline int fstat(int fd, struct stat *buf) {
    return (int)syscall2(SYS_FSTAT, fd, (long)buf);
}

static inline int mkdir(const char *path) {
    return (int)syscall1(SYS_MKDIR, (long)path);
}

static inline int rmdir(const char *path) {
    return (int)syscall1(SYS_RMDIR, (long)path);
}

static inline int unlink(const char *path) {
    return (int)syscall1(SYS_UNLINK, (long)path);
}

static inline int create(const char *path) {
    return (int)syscall1(SYS_CREATE, (long)path);
}

static inline int readdir(int fd, struct dirent *buf, int index) {
    return (int)syscall3(SYS_READDIR, fd, (long)buf, index);
}

static inline int chdir(const char *path) {
    return (int)syscall1(SYS_CHDIR, (long)path);
}

static inline int getcwd(char *buf, int size) {
    return (int)syscall2(SYS_GETCWD, (long)buf, size);
}

static inline int rename(const char *oldpath, const char *newpath) {
    return (int)syscall2(SYS_RENAME, (long)oldpath, (long)newpath);
}

static inline int seek(int fd, int offset, int whence) {
    return (int)syscall3(SYS_SEEK, fd, offset, whence);
}

static inline int yield(void) {
    return (int)syscall0(SYS_YIELD);
}

static inline int pipe(int fds[2]){
    return (int)syscall1(SYS_PIPE, (long)fds);
}
static inline int dup2(int oldfd, int newfd){
    return (int)syscall2(SYS_DUP2, oldfd, newfd);
}

static inline int fork(void) {
    return (int)syscall0(SYS_FORK);
}

static inline int exec(const char *path, char **argv) {
    return (int)syscall2(SYS_EXEC, (long)path, (long)argv);
}

static inline int waitpid(int pid) {
    return (int)syscall1(SYS_WAITPID, pid);
}

static inline int getpid(void) {
    return (int)syscall0(SYS_GETPID);
}

static inline int kill(int pid, int sig) {
    return (int)syscall2(SYS_KILL, pid, sig);
}

static inline void *signal(int sig, void *handler) {
    return (void *)syscall2(SYS_SIGNAL, sig, (long)handler);
}

static inline int setpgid(int pid, int pgid) {
    return (int)syscall2(SYS_SETPGID, pid, pgid);
}

static inline int tcsetpgrp(int pgid) {
    return (int)syscall1(SYS_TCSETPGRP, pgid);
}

static inline int tcgetpgrp(void) {
    return (int)syscall0(SYS_TCGETPGRP);
}

static inline int fb_info(struct user_fb_info *out) {
    return (int)syscall1(SYS_FB_INFO, (long)out);
}

static inline int fb_putpixel(int x, int y, unsigned int colour) {
    return (int)syscall3(SYS_FB_PUTPIXEL, x, y, (long)colour);
}

static inline int input_poll(struct user_input_event *out) {
    return (int)syscall1(SYS_INPUT_POLL, (long)out);
}

static inline unsigned long ticks(void) {
    return (unsigned long)syscall0(SYS_TICKS);
}

static inline long fb_map(void) {
    return syscall0(SYS_FB_MAP);
}

static inline int fb_present(void *buf) {
    return (int)syscall1(SYS_FB_PRESENT, (long)buf);
}

static inline int fb_present_rect(void *buf, int x, int y, int w, int h) {
    return (int)syscall5(SYS_FB_PRESENT_RECT, (long)buf, x, y, w, h);
}

// ============================================================================
// Window management
// ============================================================================

struct user_win_info {
    int active;
    int owner_pid;
    int width;
    int height;
    int dirty;
};

static inline int win_create(int flags, int w, int h) {
    return (int)syscall3(SYS_WIN_CREATE, flags, w, h);
}

static inline int win_present(int handle, int x, int y, int w, int h) {
    return (int)syscall5(SYS_WIN_PRESENT, handle, x, y, w, h);
}

static inline int win_close(int handle) {
    return (int)syscall1(SYS_WIN_CLOSE, handle);
}

static inline int win_poll(int handle, struct user_input_event *out) {
    return (int)syscall2(SYS_WIN_POLL, handle, (long)out);
}

static inline int win_info(int slot, struct user_win_info *out) {
    return (int)syscall2(SYS_WIN_INFO, slot, (long)out);
}

static inline long win_map(int handle) {
    return syscall1(SYS_WIN_MAP, handle);
}

static inline int win_send(int handle, const struct user_input_event *ev) {
    return (int)syscall2(SYS_WIN_SEND, handle, (long)ev);
}

#endif // LIBSYS_H

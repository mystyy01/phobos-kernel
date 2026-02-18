// PHOBOS Kernel

#include "idt.h"
#include "drivers/ata.h"
#include "drivers/keyboard.h"
#include "drivers/mouse.h"
#include "drivers/uhci.h"
#include "drivers/framebuffer.h"
#include "drivers/virtio_gpu.h"
#include "font.h"
#include "fs/fat32.h"
#include "fs/vfs.h"
#include "gdt.h"
#include "paging.h"
#include "pmm.h"
#include "sched.h"
#include "syscall.h"
#include "tty.h"
#include "console.h"

#define START_USER_TASK 0
#define START_SCHEDULER 1
#define START_IDLE_TASK 1

#ifndef CONFIG_ENABLE_SHELL
#define CONFIG_ENABLE_SHELL 1
#endif

extern int shell_main(void) __attribute__((weak));

// Video memory starts at 0xB8000
// Each character: 2 bytes (char + color)
// Color: 0x0F = white on black

volatile unsigned short *video = (volatile unsigned short *)0xB8000;

void print(const char *str, int row) {
    volatile unsigned short *pos = video + (row * 80);
    while (*str) {
        *pos++ = (0x0F << 8) | *str++;
    }
}

void print_color(const char *str, int row, unsigned char color) {
    volatile unsigned short *pos = video + (row * 80);
    while (*str) {
        *pos++ = (color << 8) | *str++;
    }
}

static void idle_thread(void) {
    while (1) {
        __asm__ volatile ("sti; hlt");
    }
}

void kernel_main(void) {
    print("PHOBOS - 64-bit C Kernel", 0);

    // Initialize paging with user-accessible pages
    paging_init();
    // Initialize physical memory manager (assume 2MB..64MB usable)
    pmm_init(0x200000, 0x4000000);
    // VESA stuff
    fb_init();
    uint64_t fb_addr = (uint64_t)(*(uint32_t *)0x5028);
    uint64_t bytes_per_pixel = (uint64_t)(fb_bpp() / 8);
    if (bytes_per_pixel == 0) {
        bytes_per_pixel = 4;
    }
    uint64_t fb_size = (uint64_t)fb_width() * (uint64_t)fb_height() * bytes_per_pixel;
    uint64_t map_start = fb_addr & ~0xFFFULL;
    uint64_t map_end = (fb_addr + fb_size + 0xFFFULL) & ~0xFFFULL;
    for (uint64_t addr = map_start; addr < map_end; addr += 0x1000){
        paging_map_kernel_page(paging_kernel_pml4(), addr, addr, PAGE_PRESENT | PAGE_WRITABLE);
    }
    console_init();
    // Allow user tasks to write to VGA for now
    paging_mark_user_region(0xB8000, 0x1000);

    // Initialize GDT and TSS (for ring3 stack switch)
    gdt_init();

    // Initialize scheduler structures
    sched_init();
    sched_bootstrap_current();

    // Initialize terminal state
    tty_init();

    // Initialize keyboard and interrupts
    keyboard_init();
    idt_init();
    mouse_init();
    uhci_init();
    virtio_gpu_init();

    // Initialize syscall mechanism
    syscall_init();

    // Initialize ATA and mount filesystem
    ata_init();
    ata_select_drive(ATA_DRIVE_SLAVE);

    if (fat32_init(0) == 0) {
        print_color("FAT32 mounted", 1, 0x0A);
        vfs_set_root(fat32_get_root());
        // Create standard directories
        ensure_path_exists("/apps");
        ensure_path_exists("/core");
        ensure_path_exists("/users/root");
        ensure_path_exists("/cfg");
        ensure_path_exists("/temp");
        ensure_path_exists("/dev");
    } else {
        print_color("FAT32 failed", 1, 0x0C);
    }

    // Create idle task
    if (START_IDLE_TASK) {
        struct task *idle = sched_create_kernel(idle_thread);
        if (idle) {
            idle->is_idle = 1;
        }
    }

    // Launch first user task
    if (START_USER_TASK) {
        struct vfs_node *task_a = vfs_resolve_path("/apps/ticka");
        if (task_a) {
            sched_create_user(task_a, 0);
        } else {
            print_color("ticka missing", 6, 0x0C);
        }
        struct vfs_node *task_b = vfs_resolve_path("/apps/tickb");
        if (task_b) {
            sched_create_user(task_b, 0);
        } else {
            print_color("tickb missing", 7, 0x0C);
        }
    }

    if (START_SCHEDULER) {
        sched_start();
    }
    __asm__ volatile ("sti");

    // Call user shell if it is linked in this build.
#if CONFIG_ENABLE_SHELL
    print("Starting shell...", 3);
    if (shell_main) {
        shell_main();
    } else {
        print("No shell linked.", 4);
        while (1) {
            __asm__ volatile ("sti; hlt");
        }
    }
#else
    print("Shell disabled at build.", 4);
    while (1) {
        __asm__ volatile ("sti; hlt");
    }
#endif

    // If shell exits, halt
    print("Shell exited. System halted.", 5);
    while (1) {
        __asm__ volatile ("hlt");
    }
}

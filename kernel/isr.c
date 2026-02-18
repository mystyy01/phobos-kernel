#include "isr.h"
#include "drivers/keyboard.h"
#include "drivers/mouse.h"
#include "drivers/uhci.h"
#include "sched.h"

// System tick counter (incremented by timer IRQ ~18.2 times/sec by default PIT)
volatile uint64_t system_ticks = 0;

// Video memory for exception output
static volatile unsigned short *video = (volatile unsigned short *)0xB8000;

#define SCREEN_WIDTH 80

static void print_at(const char *str, int x, int y, unsigned char color) {
    volatile unsigned short *pos = video + (y * SCREEN_WIDTH) + x;
    while (*str) {
        *pos++ = (color << 8) | *str++;
    }
}

static void print_hex(uint64_t n, int x, int y) {
    char hex[] = "0x0000000000000000";
    char *digits = "0123456789ABCDEF";
    for (int i = 17; i >= 2; i--) {
        hex[i] = digits[n & 0xF];
        n >>= 4;
    }
    print_at(hex, x, y, 0x0F);
}

static inline void dbg_out(uint8_t v) {
    __asm__ volatile ("outb %0, %1" : : "a"(v), "Nd"((uint16_t)0xE9));
}

static void dbg_str(const char *s) {
    while (*s) dbg_out((uint8_t)*s++);
}

static void dbg_hex64(uint64_t v) {
    const char *hex = "0123456789ABCDEF";
    for (int i = 15; i >= 0; i--) {
        dbg_out((uint8_t)hex[(v >> (i * 4)) & 0xF]);
    }
}

static const char *exception_names[] = {
    "Division by Zero",
    "Debug",
    "NMI",
    "Breakpoint",
    "Overflow",
    "Bound Range Exceeded",
    "Invalid Opcode",
    "Device Not Available",
    "Double Fault",
    "Coprocessor Segment Overrun",
    "Invalid TSS",
    "Segment Not Present",
    "Stack Fault",
    "General Protection Fault",
    "Page Fault",
    "Reserved",
    "x87 FPU Error",
    "Alignment Check",
    "Machine Check",
    "SIMD Floating Point",
    "Virtualization",
    "Control Protection",
    "Reserved", "Reserved", "Reserved", "Reserved",
    "Reserved", "Reserved", "Reserved", "Reserved",
    "Reserved", "Reserved"
};

void isr_handler(uint64_t int_no, struct irq_frame *frame) {
    // If a user task faulted, kill it instead of halting the whole system.
    struct task *t = sched_current();
    if (t && t->is_user) {
        dbg_str("[isr] user fault int=");
        dbg_hex64(int_no);
        if (frame) {
            dbg_str(" rip=");
            dbg_hex64(frame->rip);
            dbg_str(" err=");
            dbg_hex64(frame->err_code);
        }
        dbg_str(" pid=");
        dbg_hex64(t->id);
        if (int_no == 14) {
            uint64_t cr2;
            __asm__ volatile ("mov %%cr2, %0" : "=r"(cr2));
            dbg_str(" cr2=");
            dbg_hex64(cr2);
        }
        dbg_str("\n");

        // Print info to VGA for debugging
        print_at("USER FAULT: ", 0, 5, 0x0C);
        if (int_no < 32) {
            print_at(exception_names[int_no], 12, 5, 0x0C);
        }
        print_at("PID: ", 0, 6, 0x0E);
        print_hex(t->id, 5, 6);
        if (int_no == 14) {
            uint64_t cr2;
            __asm__ volatile ("mov %%cr2, %0" : "=r"(cr2));
            print_at("CR2: ", 0, 7, 0x0C);
            print_hex(cr2, 5, 7);
        }

        // Kill the task — wakes parent in sched_waitpid, scheduler continues
        sched_exit(-1);
        __builtin_unreachable();
    }

    // Kernel fault — display error and halt (unrecoverable)
    print_at("EXCEPTION: ", 0, 5, 0x0C);
    if (int_no < 32) {
        print_at(exception_names[int_no], 11, 5, 0x0C);
    }
    print_at("INT#: ", 0, 6, 0x0C);
    print_hex(int_no, 6, 6);

    if (int_no == 14) {
        uint64_t cr2;
        __asm__ volatile ("mov %%cr2, %0" : "=r"(cr2));
        print_at("CR2: ", 0, 7, 0x0C);
        print_hex(cr2, 5, 7);
        print_at("(Faulting address)", 24, 7, 0x0E);
    }

    while (1) {
        __asm__ volatile ("hlt");
    }
}

struct irq_frame *irq_handler(uint64_t int_no, struct irq_frame *frame) {
    if (int_no == 32) {
        // Timer interrupt - increment system tick counter and schedule
        system_ticks++;
        uhci_poll();
        frame = sched_tick(frame);
    } else if (int_no == 33) {
        // Keyboard interrupt - read scancode and pass to keyboard driver
        uint8_t scancode;
        __asm__ volatile ("inb %1, %0" : "=a"(scancode) : "Nd"((uint16_t)0x60));
        keyboard_handle_scancode(scancode);
    } else if (int_no == 44) {
        // Mouse interrupt - read data byte and feed PS/2 packet parser.
        uint8_t data_byte;
        __asm__ volatile ("inb %1, %0" : "=a"(data_byte) : "Nd"((uint16_t)0x60));
        mouse_handle_byte(data_byte);
    }

    // Send End of Interrupt (EOI) to PIC
    if (int_no >= 40) {
        __asm__ volatile ("outb %0, %1" : : "a"((uint8_t)0x20), "Nd"((uint16_t)0xA0));
    }
    __asm__ volatile ("outb %0, %1" : : "a"((uint8_t)0x20), "Nd"((uint16_t)0x20));

    return frame;
}

#include "mouse.h"
#include "framebuffer.h"

#define MOUSE_BUFFER_SIZE 64

static struct mouse_event mouse_buffer[MOUSE_BUFFER_SIZE];
static volatile int mouse_read_idx = 0;
static volatile int mouse_write_idx = 0;

static uint8_t packet[3];
static int packet_idx = 0;

static int mouse_x = 0;
static int mouse_y = 0;
static uint8_t mouse_buttons = 0;

static inline uint8_t inb(uint16_t port) {
    uint8_t value;
    __asm__ volatile ("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static inline void outb(uint16_t port, uint8_t value) {
    __asm__ volatile ("outb %0, %1" : : "a"(value), "Nd"(port));
}

static int ps2_wait_write(void) {
    for (int i = 0; i < 100000; i++) {
        if ((inb(0x64) & 0x02) == 0) {
            return 1;
        }
    }
    return 0;
}

static int ps2_wait_read(void) {
    for (int i = 0; i < 100000; i++) {
        if (inb(0x64) & 0x01) {
            return 1;
        }
    }
    return 0;
}

static int mouse_write_cmd(uint8_t value) {
    if (!ps2_wait_write()) return 0;
    outb(0x64, 0xD4);
    if (!ps2_wait_write()) return 0;
    outb(0x60, value);
    return 1;
}

static int mouse_read_data(uint8_t *value) {
    if (!value) return 0;
    if (!ps2_wait_read()) return 0;
    *value = inb(0x60);
    return 1;
}

static void mouse_queue_event(const struct mouse_event *ev) {
    int next_write = (mouse_write_idx + 1) % MOUSE_BUFFER_SIZE;
    if (next_write == mouse_read_idx) {
        return; // drop when full
    }
    mouse_buffer[mouse_write_idx] = *ev;
    mouse_write_idx = next_write;
}

void mouse_init(void) {
    mouse_read_idx = 0;
    mouse_write_idx = 0;
    packet_idx = 0;
    mouse_buttons = 0;

    int w = fb_width();
    int h = fb_height();
    mouse_x = (w > 0) ? (w / 2) : 0;
    mouse_y = (h > 0) ? (h / 2) : 0;

    // Flush any pending data in the controller output buffer.
    for (int i = 0; i < 32; i++) {
        if ((inb(0x64) & 0x01) == 0) break;
        (void)inb(0x60);
    }

    // Enable auxiliary (mouse) device.
    if (!ps2_wait_write()) return;
    outb(0x64, 0xA8);

    // Read controller command byte.
    if (!ps2_wait_write()) return;
    outb(0x64, 0x20);
    uint8_t status = 0;
    if (!mouse_read_data(&status)) return;

    // Enable IRQ12 and mouse clock.
    status |= 0x02;
    status &= (uint8_t)~0x20;

    if (!ps2_wait_write()) return;
    outb(0x64, 0x60);
    if (!ps2_wait_write()) return;
    outb(0x60, status);

    // Reset defaults, then enable packet streaming.
    uint8_t ack;
    if (mouse_write_cmd(0xF6)) (void)mouse_read_data(&ack);
    if (mouse_write_cmd(0xF4)) (void)mouse_read_data(&ack);
}

void mouse_handle_byte(uint8_t data_byte) {
    if (packet_idx == 0 && (data_byte & 0x08) == 0) {
        return; // out-of-sync; wait for a valid first byte
    }

    packet[packet_idx++] = data_byte;
    if (packet_idx < 3) {
        return;
    }
    packet_idx = 0;

    int8_t dx = (int8_t)packet[1];
    int8_t dy = (int8_t)packet[2];

    if (packet[0] & 0x40) dx = 0; // x overflow
    if (packet[0] & 0x80) dy = 0; // y overflow

    if (dx != 0 || dy != 0) {
        mouse_x += (int)dx;
        mouse_y -= (int)dy; // PS/2 y is positive up; screen y is positive down

        int max_x = fb_width() - 1;
        int max_y = fb_height() - 1;
        if (mouse_x < 0) mouse_x = 0;
        if (mouse_y < 0) mouse_y = 0;
        if (max_x < 0) max_x = 0;
        if (max_y < 0) max_y = 0;
        if (mouse_x > max_x) mouse_x = max_x;
        if (mouse_y > max_y) mouse_y = max_y;

        struct mouse_event move_ev;
        move_ev.type = MOUSE_EVENT_MOVE;
        move_ev.x = (int16_t)mouse_x;
        move_ev.y = (int16_t)mouse_y;
        move_ev.buttons = packet[0] & 0x07;
        move_ev.button = 0;
        move_ev.pressed = 0;
        mouse_queue_event(&move_ev);
    }

    uint8_t new_buttons = packet[0] & 0x07;
    uint8_t changed = (uint8_t)(new_buttons ^ mouse_buttons);
    if (changed) {
        for (int b = 0; b < 3; b++) {
            uint8_t mask = (uint8_t)(1u << b);
            if ((changed & mask) == 0) continue;

            struct mouse_event btn_ev;
            btn_ev.type = MOUSE_EVENT_BUTTON;
            btn_ev.x = (int16_t)mouse_x;
            btn_ev.y = (int16_t)mouse_y;
            btn_ev.buttons = new_buttons;
            btn_ev.button = (uint8_t)(b + 1);
            btn_ev.pressed = (new_buttons & mask) ? 1u : 0u;
            mouse_queue_event(&btn_ev);
        }
    }

    mouse_buttons = new_buttons;
}

int mouse_poll_event(struct mouse_event *out) {
    if (!out) return 0;
    if (mouse_read_idx == mouse_write_idx) {
        return 0;
    }
    *out = mouse_buffer[mouse_read_idx];
    mouse_read_idx = (mouse_read_idx + 1) % MOUSE_BUFFER_SIZE;
    return 1;
}

int mouse_get_x(void) {
    return mouse_x;
}

int mouse_get_y(void) {
    return mouse_y;
}

uint8_t mouse_get_buttons(void) {
    return mouse_buttons;
}

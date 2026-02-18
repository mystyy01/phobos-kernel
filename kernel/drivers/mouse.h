#ifndef MOUSE_H
#define MOUSE_H

#include <stdint.h>

#define MOUSE_EVENT_MOVE   1
#define MOUSE_EVENT_BUTTON 2

struct mouse_event {
    uint8_t type;      // MOUSE_EVENT_*
    int16_t x;         // absolute screen-space cursor x
    int16_t y;         // absolute screen-space cursor y
    uint8_t buttons;   // bitmask: bit0 left, bit1 right, bit2 middle
    uint8_t button;    // 1 left, 2 right, 3 middle (button events only)
    uint8_t pressed;   // 1 press, 0 release (button events only)
};

void mouse_init(void);
void mouse_handle_byte(uint8_t data_byte);
int mouse_poll_event(struct mouse_event *out);
int mouse_get_x(void);
int mouse_get_y(void);
uint8_t mouse_get_buttons(void);

#endif

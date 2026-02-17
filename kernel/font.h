#ifndef FONT_H
#define FONT_H

#include <stdint.h>

struct font{
    // needs font data
    const uint8_t *font_addr;
    // width
    uint8_t width;
    // height
    uint8_t height;
    // first and last char code it covers (print some sort of ? for others)
    uint8_t first_char_code;
    uint8_t last_char_code;

};

extern struct font default_font;

void font_draw_char(struct font *font, char c, int x, int y, uint32_t colour);
void font_draw_string(struct font *font, const char *str, int x, int y, uint32_t colour);


#endif

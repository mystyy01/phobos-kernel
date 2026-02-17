#include "console.h"
#include "font.h"
#include "drivers/framebuffer.h"

#define CONSOLE_COLS 80
#define CONSOLE_ROWS 25
#define CONSOLE_FG 0xFFFFFF
#define CONSOLE_BG 0x000000

static int cursor_row = 0;
static int cursor_col = 0;
static char console_chars[CONSOLE_ROWS][CONSOLE_COLS];

static void fill_rect(int x, int y, int w, int h, uint32_t colour) {
    for (int yy = 0; yy < h; yy++) {
        for (int xx = 0; xx < w; xx++) {
            fb_putpixel(x + xx, y + yy, colour);
        }
    }
}

static void draw_cell(int row, int col, char ch) {
    int cell_x = col * default_font.width;
    int cell_y = row * default_font.height;

    fill_rect(cell_x, cell_y, default_font.width, default_font.height, CONSOLE_BG);
    font_draw_char(&default_font, ch, cell_x, cell_y, CONSOLE_FG);
}

static void redraw_all(void) {
    for (int row = 0; row < CONSOLE_ROWS; row++) {
        for (int col = 0; col < CONSOLE_COLS; col++) {
            draw_cell(row, col, console_chars[row][col]);
        }
    }
}

static void scroll_up(void) {
    for (int row = 1; row < CONSOLE_ROWS; row++) {
        for (int col = 0; col < CONSOLE_COLS; col++) {
            console_chars[row - 1][col] = console_chars[row][col];
        }
    }
    for (int col = 0; col < CONSOLE_COLS; col++) {
        console_chars[CONSOLE_ROWS - 1][col] = ' ';
    }
    redraw_all();
    cursor_row = CONSOLE_ROWS - 1;
}

void console_init(void) {
    console_clear();
}

void console_clear(void) {
    for (int row = 0; row < CONSOLE_ROWS; row++) {
        for (int col = 0; col < CONSOLE_COLS; col++) {
            console_chars[row][col] = ' ';
        }
    }
    fill_rect(0, 0, CONSOLE_COLS * default_font.width, CONSOLE_ROWS * default_font.height, CONSOLE_BG);
    cursor_row = 0;
    cursor_col = 0;
}

void console_putc(int c) {
    if (c == '\n') {
        cursor_col = 0;
        cursor_row++;
    } else if (c == '\r') {
        cursor_col = 0;
    } else if (c == '\b') {
        if (cursor_col > 0) {
            cursor_col--;
            console_chars[cursor_row][cursor_col] = ' ';
            draw_cell(cursor_row, cursor_col, ' ');
        }
    } else if (c == '\t') {
        int next = (cursor_col + 8) & ~7;
        while (cursor_col < next) {
            console_chars[cursor_row][cursor_col] = ' ';
            draw_cell(cursor_row, cursor_col, ' ');
            cursor_col++;
            if (cursor_col >= CONSOLE_COLS) {
                cursor_col = 0;
                cursor_row++;
                break;
            }
        }
    } else {
        if (c < 0x20 || c > 0x7E) {
            c = '?';
        }
        console_chars[cursor_row][cursor_col] = (char)c;
        draw_cell(cursor_row, cursor_col, (char)c);
        cursor_col++;
    }

    if (cursor_col >= CONSOLE_COLS) {
        cursor_col = 0;
        cursor_row++;
    }
    if (cursor_row >= CONSOLE_ROWS) {
        scroll_up();
    }
}

int console_write(const char *buf, int count) {
    if (!buf || count < 0) {
        return -1;
    }
    for (int i = 0; i < count; i++) {
        console_putc((unsigned char)buf[i]);
    }
    return count;
}

void console_get_cursor(int *row, int *col) {
    if (row) {
        *row = cursor_row;
    }
    if (col) {
        *col = cursor_col;
    }
}

void console_set_cursor(int row, int col) {
    if (row < 0) {
        row = 0;
    }
    if (col < 0) {
        col = 0;
    }
    if (row >= CONSOLE_ROWS) {
        row = CONSOLE_ROWS - 1;
    }
    if (col >= CONSOLE_COLS) {
        col = CONSOLE_COLS - 1;
    }
    cursor_row = row;
    cursor_col = col;
}

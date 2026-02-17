#ifndef CONSOLE_H
#define CONSOLE_H

#include <stdint.h>

void console_init(void);
void console_clear(void);
void console_putc(int c);
int console_write(const char *buf, int count);
void console_get_cursor(int *row, int *col);
void console_set_cursor(int row, int col);

#endif

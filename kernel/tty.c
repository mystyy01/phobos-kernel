#include "tty.h"

// Global TTY state (single terminal for now)
static int foreground_pgid = 0;
static int tty_mode = TTY_MODE_COOKED;

void tty_init(void) {
    foreground_pgid = 0;
    tty_mode = TTY_MODE_COOKED;
}

int tty_get_foreground_pgid(void) {
    return foreground_pgid;
}

void tty_set_foreground_pgid(int pgid) {
    foreground_pgid = pgid;
}

int tty_get_mode(void) {
    return tty_mode;
}

void tty_set_mode(int mode) {
    tty_mode = mode;
}

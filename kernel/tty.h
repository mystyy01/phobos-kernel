#ifndef TTY_H
#define TTY_H

// Terminal (TTY) modes
#define TTY_MODE_COOKED 0  // Default: line editing, Ctrl+C sends SIGINT
#define TTY_MODE_RAW    1  // Pass all keys through as-is

// Initialize TTY state
void tty_init(void);

// Get/Set foreground process group
int tty_get_foreground_pgid(void);
void tty_set_foreground_pgid(int pgid);

// Get/Set terminal mode
int tty_get_mode(void);
void tty_set_mode(int mode);

#endif

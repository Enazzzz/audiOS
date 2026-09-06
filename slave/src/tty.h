#ifndef AUDIOS_TTY_H
#define AUDIOS_TTY_H

#include <stdint.h>

#define TTY_COL_BG	0x101014u
#define TTY_COL_FG	0xD6DCE0u
#define TTY_COL_DIM	0x8A9098u
#define TTY_COL_ACCENT	0x7EC8C8u
#define TTY_COL_AUDIO	0xE8B86Du
#define TTY_COL_ERR	0xE07070u
#define TTY_COL_SEL_BG	0x3A5A72u
#define TTY_COL_SEL_FG	0xF4F8FCu

/** VGA 80×25 + COM1. */
void tty_init(void);
void tty_set_color(uint32_t rgb);
void tty_putc(char c);
void tty_puts(const char *s);
void tty_printf(const char *fmt, ...);
void tty_clear(void);
unsigned tty_cols(void);
unsigned tty_rows(void);
unsigned tty_cursor_col(void);
unsigned tty_cursor_row(void);
void tty_set_cursor(unsigned col, unsigned row);
void tty_put_xy(unsigned col, unsigned row, char ch, uint32_t rgb);
void tty_cell_at(unsigned col, unsigned row, unsigned char *ch, uint32_t *rgb);

#endif

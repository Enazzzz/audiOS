#ifndef AUDIOS_TTY_H
#define AUDIOS_TTY_H

#include <limine.h>
#include <stdint.h>

/** Packed RGB colours used by the console (logical, before mask packing). */
#define TTY_COL_BG	0x101014u
#define TTY_COL_FG	0xD6DCE0u
#define TTY_COL_DIM	0x8A9098u
#define TTY_COL_ACCENT	0x7EC8C8u
#define TTY_COL_AUDIO	0xE8B86Du
#define TTY_COL_ERR	0xE07070u

/** Attach the console to a Limine framebuffer and clear the screen. */
void tty_init(struct limine_framebuffer *fb);

/** Set the current foreground colour (framebuffer + serial ANSI). */
void tty_set_color(uint32_t rgb);

/** Write one byte, handling newline, tab, backspace, and UTF-8 bullet. */
void tty_putc(char c);

/** Write a NUL-terminated string. */
void tty_puts(const char *s);

/** Formatted print to the console. */
void tty_printf(const char *fmt, ...);

/** Fill the screen and reset the cursor. */
void tty_clear(void);

/**
 * Plot one glyph at a grid cell without moving the console cursor.
 * Used by full-screen games so a 60 Hz redraw does not flood serial
 * with a full `tty_clear`. Unchanged cells are skipped.
 */
void tty_put_xy(unsigned col, unsigned row, char ch, uint32_t rgb);

/** Scroll the console view by one history line (hold-repeat uses this). */
void tty_line_up(void);
void tty_line_down(void);

/** Page the console through RAM scrollback (does not change the live buffer). */
void tty_page_up(void);
void tty_page_down(void);

/** Hide the underline cursor (editor / games). */
void tty_cursor_hide(void);

/** Framebuffer size Limine actually set (pixels). */
unsigned tty_fb_width(void);
unsigned tty_fb_height(void);

/** Jump the view back to the live prompt line. */
void tty_view_live(void);

/** Text grid size after `tty_init`. */
unsigned tty_cols(void);
unsigned tty_rows(void);

/** Draw or erase the hardware cursor bar using the PIT clock. */
void tty_tick_cursor(uint64_t ticks);

/**
 * Run `fn` during slow console work (scroll / long prints) so HDA DMA
 * is refilled instead of underrunning.
 */
void tty_set_idle(void (*fn)(void));

#endif

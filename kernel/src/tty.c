#include "tty.h"
#include "font.h"
#include "klib.h"
#include "serial.h"

#include <stdarg.h>
#include <stddef.h>

static struct limine_framebuffer *fb;
static uint32_t packed_bg;
static uint32_t packed_fg;
static size_t cursor_col;
static size_t cursor_row;
static size_t cols;
static size_t rows;
static int utf8_need;
static unsigned utf8_index;
static uint8_t utf8_buf[4];
static int cursor_visible;

/** Write bytes to COM1 without interpreting console state. */
static void serial_puts_raw(const char *s)
{
	while (*s != '\0') {
		serial_putc(*s++);
	}
}

/** Pack a 0xRRGGBB colour using the framebuffer channel masks. */
static uint32_t pack_rgb(uint32_t rgb)
{
	uint32_t r = (rgb >> 16) & 0xFF;
	uint32_t g = (rgb >> 8) & 0xFF;
	uint32_t b = rgb & 0xFF;
	return (r << fb->red_mask_shift) | (g << fb->green_mask_shift) | (b << fb->blue_mask_shift);
}

/** Plot one 8x16 glyph at the current cursor without moving it. */
static void plot_glyph(unsigned char ch)
{
	if (fb == NULL) {
		return;
	}
	if (ch >= FONT_GLYPHS) {
		ch = '?';
	}
	size_t x0 = cursor_col * FONT_WIDTH;
	size_t y0 = cursor_row * FONT_HEIGHT;
	uint8_t *base = (uint8_t *)fb->address;
	size_t pitch = (size_t)fb->pitch;
	unsigned bytes = (unsigned)((fb->bpp + 7) / 8);
	for (size_t row = 0; row < FONT_HEIGHT; row++) {
		uint8_t bits = font8x16[ch][row];
		for (size_t col = 0; col < FONT_WIDTH; col++) {
			uint32_t colour = (bits & (1u << col)) ? packed_fg : packed_bg;
			uint8_t *pixel = base + (y0 + row) * pitch + (x0 + col) * bytes;
			if (bytes >= 4) {
				*(uint32_t *)pixel = colour;
			} else if (bytes == 3) {
				pixel[0] = (uint8_t)(colour & 0xFF);
				pixel[1] = (uint8_t)((colour >> 8) & 0xFF);
				pixel[2] = (uint8_t)((colour >> 16) & 0xFF);
			}
		}
	}
}

/** Scroll the framebuffer up one text row. */
static void tty_scroll(void)
{
	if (fb == NULL || rows == 0) {
		return;
	}
	uint8_t *base = (uint8_t *)fb->address;
	size_t pitch = (size_t)fb->pitch;
	size_t row_bytes = pitch * FONT_HEIGHT;
	memmove(base, base + row_bytes, row_bytes * (rows - 1));
	uint8_t *last = base + row_bytes * (rows - 1);
	for (size_t y = 0; y < FONT_HEIGHT; y++) {
		uint32_t *line = (uint32_t *)(last + y * pitch);
		for (size_t x = 0; x < fb->width; x++) {
			line[x] = packed_bg;
		}
	}
	cursor_row = rows - 1;
	cursor_col = 0;
}

/** Draw or erase the underline at the current cell. */
static void tty_paint_cursor(int show)
{
	if (fb == NULL || cursor_col >= cols || cursor_row >= rows) {
		return;
	}
	size_t x0 = cursor_col * FONT_WIDTH;
	size_t y = cursor_row * FONT_HEIGHT + FONT_HEIGHT - 2;
	uint8_t *base = (uint8_t *)fb->address;
	uint32_t colour = show ? packed_fg : packed_bg;
	unsigned bytes = (unsigned)((fb->bpp + 7) / 8);
	if (bytes < 4) {
		cursor_visible = show;
		return;
	}
	for (size_t col = 0; col < FONT_WIDTH; col++) {
		uint8_t *pixel = base + y * (size_t)fb->pitch + (x0 + col) * bytes;
		*(uint32_t *)pixel = colour;
	}
	cursor_visible = show;
}

/** Hide the cursor bar before mutating the cell it occupies. */
static void tty_hide_cursor(void)
{
	if (cursor_visible) {
		tty_paint_cursor(0);
	}
}

/** Advance after a glyph, wrapping and scrolling as needed. */
static void tty_advance(void)
{
	cursor_col++;
	if (cursor_col >= cols) {
		cursor_col = 0;
		cursor_row++;
	}
	if (cursor_row >= rows) {
		tty_scroll();
	}
}

/** Attach to the framebuffer, compute the text grid, and clear the screen. */
void tty_init(struct limine_framebuffer *framebuffer)
{
	fb = framebuffer;
	packed_bg = pack_rgb(TTY_COL_BG);
	packed_fg = pack_rgb(TTY_COL_FG);
	cols = (size_t)fb->width / FONT_WIDTH;
	rows = (size_t)fb->height / FONT_HEIGHT;
	if (cols == 0) {
		cols = 1;
	}
	if (rows == 0) {
		rows = 1;
	}
	tty_clear();
}

/** Update framebuffer and serial colour together. */
void tty_set_color(uint32_t rgb)
{
	if (fb != NULL) {
		packed_fg = pack_rgb(rgb);
	}
	if (rgb == TTY_COL_ACCENT) {
		serial_puts_raw("\033[36m");
	} else if (rgb == TTY_COL_AUDIO) {
		serial_puts_raw("\033[33m");
	} else if (rgb == TTY_COL_DIM) {
		serial_puts_raw("\033[90m");
	} else if (rgb == TTY_COL_ERR) {
		serial_puts_raw("\033[31m");
	} else {
		serial_puts_raw("\033[0m");
	}
}

/** Fill the framebuffer with the background colour and home the cursor. */
void tty_clear(void)
{
	tty_hide_cursor();
	if (fb != NULL) {
		uint8_t *base = (uint8_t *)fb->address;
		for (uint64_t y = 0; y < fb->height; y++) {
			uint32_t *line = (uint32_t *)(base + y * fb->pitch);
			for (uint64_t x = 0; x < fb->width; x++) {
				line[x] = packed_bg;
			}
		}
	}
	cursor_col = 0;
	cursor_row = 0;
	utf8_need = 0;
	utf8_index = 0;
	serial_puts_raw("\033[2J\033[H");
}

/** Emit a decoded glyph, including the identity bullet at FONT_BULLET. */
static void tty_emit(unsigned char ch)
{
	tty_hide_cursor();
	if (ch == '\n') {
		serial_putc('\n');
		cursor_col = 0;
		cursor_row++;
		if (cursor_row >= rows) {
			tty_scroll();
		}
		return;
	}
	if (ch == '\r') {
		cursor_col = 0;
		return;
	}
	if (ch == '\t') {
		size_t next = (cursor_col + 8) & ~(size_t)7;
		while (cursor_col < next) {
			tty_emit(' ');
		}
		return;
	}
	if (ch == '\b') {
		if (cursor_col > 0) {
			cursor_col--;
			plot_glyph(' ');
			serial_puts_raw("\b \b");
		}
		return;
	}
	plot_glyph(ch);
	if (ch == FONT_BULLET) {
		serial_puts_raw("\xE2\x80\xA2");
	} else if (ch >= 32 && ch < 127) {
		serial_putc((char)ch);
	}
	tty_advance();
}

/**
 * Write one byte. UTF-8 for U+2022 (bullet) is decoded into FONT_BULLET;
 * other non-ASCII sequences are dropped so the console stays determinate.
 */
void tty_putc(char c)
{
	unsigned char b = (unsigned char)c;
	if (utf8_need > 0) {
		utf8_buf[utf8_index++] = b;
		utf8_need--;
		if (utf8_need == 0) {
			if (utf8_index == 3 && utf8_buf[0] == 0xE2 && utf8_buf[1] == 0x80 && utf8_buf[2] == 0xA2) {
				tty_emit(FONT_BULLET);
			}
			utf8_index = 0;
		}
		return;
	}
	if (b == 0xE2) {
		utf8_buf[0] = b;
		utf8_index = 1;
		utf8_need = 2;
		return;
	}
	tty_emit(b);
}

/** Write a C string through `tty_putc`. */
void tty_puts(const char *s)
{
	while (*s != '\0') {
		tty_putc(*s++);
	}
}

/** Format and write to the console. */
void tty_printf(const char *fmt, ...)
{
	char buf[256];
	va_list ap;
	va_start(ap, fmt);
	kvsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	tty_puts(buf);
}

/** Blink the cursor at 2 Hz from the PIT tick count. */
void tty_tick_cursor(uint64_t ticks)
{
	if (fb == NULL) {
		return;
	}
	int show = ((ticks / 250) % 2) == 0;
	if (show != cursor_visible) {
		tty_paint_cursor(show);
	}
}

#include "tty.h"
#include "klib.h"
#include "serial.h"

#include <stdarg.h>
#include <stdint.h>

#define VGA_W	80u
#define VGA_H	25u

static volatile uint16_t *vga = (volatile uint16_t *)0xB8000;
static unsigned col;
static unsigned row;
static uint8_t attr = 0x07;
static uint8_t cells[VGA_W * VGA_H];

/** Map a logical RGB to a VGA attribute (fg on black). */
static uint8_t attr_of(uint32_t rgb)
{
	if (rgb == TTY_COL_ERR) {
		return 0x0C;
	}
	if (rgb == TTY_COL_AUDIO) {
		return 0x0E;
	}
	if (rgb == TTY_COL_ACCENT) {
		return 0x0B;
	}
	if (rgb == TTY_COL_DIM) {
		return 0x08;
	}
	return 0x07;
}

/** Scroll the VGA buffer up one row. */
static void scroll(void)
{
	unsigned i;
	for (i = 0; i < VGA_W * (VGA_H - 1u); i++) {
		vga[i] = vga[i + VGA_W];
		cells[i] = cells[i + VGA_W];
	}
	for (i = 0; i < VGA_W; i++) {
		unsigned p = (VGA_H - 1u) * VGA_W + i;
		vga[p] = (uint16_t)attr << 8 | ' ';
		cells[p] = ' ';
	}
}

void tty_init(void)
{
	serial_init();
	tty_clear();
}

void tty_set_color(uint32_t rgb)
{
	attr = attr_of(rgb);
}

void tty_putc(char c)
{
	serial_putc(c);
	if (c == '\n') {
		col = 0;
		row++;
		if (row >= VGA_H) {
			scroll();
			row = VGA_H - 1u;
		}
		return;
	}
	if (c == '\r') {
		col = 0;
		return;
	}
	if (c == '\b') {
		if (col > 0) {
			col--;
		}
		return;
	}
	{
		unsigned p = row * VGA_W + col;
		vga[p] = (uint16_t)((attr << 8) | (uint8_t)c);
		cells[p] = (uint8_t)c;
	}
	col++;
	if (col >= VGA_W) {
		col = 0;
		row++;
		if (row >= VGA_H) {
			scroll();
			row = VGA_H - 1u;
		}
	}
}

void tty_puts(const char *s)
{
	while (*s) {
		tty_putc(*s++);
	}
}

void tty_printf(const char *fmt, ...)
{
	char buf[256];
	va_list ap;
	va_start(ap, fmt);
	kvsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	tty_puts(buf);
}

void tty_clear(void)
{
	unsigned i;
	for (i = 0; i < VGA_W * VGA_H; i++) {
		vga[i] = (uint16_t)attr << 8 | ' ';
		cells[i] = ' ';
	}
	col = 0;
	row = 0;
}

unsigned tty_cols(void)
{
	return VGA_W;
}

unsigned tty_rows(void)
{
	return VGA_H;
}

unsigned tty_cursor_col(void)
{
	return col;
}

unsigned tty_cursor_row(void)
{
	return row;
}

void tty_set_cursor(unsigned c, unsigned r)
{
	if (c < VGA_W) {
		col = c;
	}
	if (r < VGA_H) {
		row = r;
	}
}

void tty_put_xy(unsigned c, unsigned r, char ch, uint32_t rgb)
{
	uint8_t a;
	unsigned p;
	if (c >= VGA_W || r >= VGA_H) {
		return;
	}
	a = attr_of(rgb);
	p = r * VGA_W + c;
	vga[p] = (uint16_t)((a << 8) | (uint8_t)ch);
	cells[p] = (uint8_t)ch;
}

void tty_cell_at(unsigned c, unsigned r, unsigned char *ch, uint32_t *rgb)
{
	if (c >= VGA_W || r >= VGA_H) {
		if (ch) {
			*ch = ' ';
		}
		if (rgb) {
			*rgb = TTY_COL_FG;
		}
		return;
	}
	if (ch) {
		*ch = cells[r * VGA_W + c];
	}
	if (rgb) {
		*rgb = TTY_COL_FG;
	}
}

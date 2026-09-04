#include "edit.h"
#include "audio.h"
#include "fat.h"
#include "fs.h"
#include "kbd.h"
#include "klib.h"
#include "serial.h"
#include "tty.h"

#include <stddef.h>

#define EDIT_MAX	(48u * 1024u)

static char path[FAT_PATH_MAX];
static char *buf;
static uint32_t cap;
static uint32_t len;
static uint32_t cur;
static int dirty;
static int running;

/** First byte of the line that contains `pos`. */
static uint32_t line_start(uint32_t pos)
{
	while (pos > 0 && buf[pos - 1] != '\n') {
		pos--;
	}
	return pos;
}

/** One past the last byte of the line (index of `\n` or `len`). */
static uint32_t line_end(uint32_t pos)
{
	while (pos < len && buf[pos] != '\n') {
		pos++;
	}
	return pos;
}

/** Insert `ch` at the cursor. */
static void insert_char(char ch)
{
	if (len + 1 >= cap) {
		return;
	}
	memmove(buf + cur + 1, buf + cur, len - cur);
	buf[cur] = ch;
	cur++;
	len++;
	dirty = 1;
}

/** Delete the character before the cursor. */
static void backspace(void)
{
	if (cur == 0) {
		return;
	}
	memmove(buf + cur - 1, buf + cur, len - cur);
	cur--;
	len--;
	dirty = 1;
}

/** Save through the VFS. */
static void save_file(void)
{
	if (!fs_write_file(path, buf, len)) {
		tty_set_color(TTY_COL_ERR);
		tty_printf("\nsave failed: %s\n", fs_error());
		tty_set_color(TTY_COL_FG);
		return;
	}
	dirty = 0;
}

/** Paint the buffer into the console. */
static void paint(void)
{
	unsigned cols = tty_cols();
	unsigned rows = tty_rows();
	if (cols == 0) {
		cols = 80;
	}
	if (rows < 3) {
		rows = 3;
	}
	unsigned text_rows = rows - 2;
	uint32_t ls = line_start(cur);
	unsigned cur_line = 0;
	for (uint32_t i = 0; i < ls; i++) {
		if (buf[i] == '\n') {
			cur_line++;
		}
	}
	unsigned top = 0;
	if (cur_line >= text_rows) {
		top = cur_line - (text_rows - 1);
	}
	tty_clear();
	tty_set_color(TTY_COL_ACCENT);
	tty_printf("edit %s%s  ^O save  ^X quit\n", path, dirty ? "*" : "");
	tty_set_color(TTY_COL_FG);
	unsigned shown = 0;
	unsigned line_i = 0;
	uint32_t i = 0;
	while (i < len && shown < text_rows) {
		uint32_t e = line_end(i);
		if (line_i >= top) {
			uint32_t n = e - i;
			if (n > cols) {
				n = cols;
			}
			for (uint32_t k = 0; k < n; k++) {
				char ch = buf[i + k];
				tty_putc((ch == '\t') ? ' ' : ch);
			}
			tty_putc('\n');
			shown++;
		}
		i = (e < len && buf[e] == '\n') ? e + 1 : e;
		line_i++;
		if (e >= len) {
			break;
		}
	}
	tty_set_color(TTY_COL_DIM);
	tty_printf("%u/%u bytes\n", cur, len);
	tty_set_color(TTY_COL_FG);
}

/** Move the cursor by `delta` lines, preserving column. */
static void move_vert(int delta)
{
	uint32_t ls = line_start(cur);
	uint32_t col = cur - ls;
	if (delta < 0) {
		if (ls == 0) {
			cur = 0;
			return;
		}
		uint32_t prev = line_start(ls - 1);
		uint32_t pe = line_end(prev);
		uint32_t w = pe - prev;
		cur = prev + (col < w ? col : w);
		return;
	}
	uint32_t e = line_end(cur);
	if (e >= len) {
		cur = len;
		return;
	}
	uint32_t ns = e + 1;
	uint32_t ne = line_end(ns);
	uint32_t w = ne - ns;
	cur = ns + (col < w ? col : w);
}

/** Handle one key. */
static void feed(int c)
{
	if (c == 15) {	/* Ctrl-O */
		save_file();
		paint();
		return;
	}
	if (c == 24) {	/* Ctrl-X */
		running = 0;
		return;
	}
	if (c == KBD_LEFT) {
		if (cur > 0) {
			cur--;
		}
		paint();
		return;
	}
	if (c == KBD_RIGHT) {
		if (cur < len) {
			cur++;
		}
		paint();
		return;
	}
	if (c == KBD_UP) {
		move_vert(-1);
		paint();
		return;
	}
	if (c == KBD_DOWN) {
		move_vert(1);
		paint();
		return;
	}
	if (c == KBD_HOME) {
		cur = line_start(cur);
		paint();
		return;
	}
	if (c == KBD_END) {
		cur = line_end(cur);
		paint();
		return;
	}
	if (c == KBD_PGUP) {
		unsigned n = tty_rows() / 2u;
		if (n == 0) {
			n = 1;
		}
		while (n--) {
			move_vert(-1);
		}
		paint();
		return;
	}
	if (c == KBD_PGDN) {
		unsigned n = tty_rows() / 2u;
		if (n == 0) {
			n = 1;
		}
		while (n--) {
			move_vert(1);
		}
		paint();
		return;
	}
	if (c == '\b' || c == 0x7F) {
		backspace();
		paint();
		return;
	}
	if (c == '\r' || c == '\n') {
		insert_char('\n');
		paint();
		return;
	}
	if (c >= 32 && c <= 126) {
		insert_char((char)c);
		paint();
		return;
	}
}

void edit_cmd(int argc, char **argv)
{
	if (argc < 2) {
		tty_puts("usage: edit <file>   (^O save, ^X quit)\n");
		return;
	}
	uint32_t iocap = 0;
	buf = (char *)fs_iobuf(&iocap);
	if (buf == NULL || iocap < 4096) {
		tty_puts("edit: no buffer\n");
		return;
	}
	cap = iocap < EDIT_MAX ? iocap : EDIT_MAX;
	ksnprintf(path, sizeof(path), "%s", argv[1]);
	len = 0;
	cur = 0;
	dirty = 0;
	uint32_t n = 0;
	if (fs_read_file(path, buf, cap - 1u, &n) && n > 0) {
		len = n;
		if (len >= cap) {
			len = cap - 1;
		}
	}
	buf[len] = '\0';
	running = 1;
	paint();
	while (running) {
		int c;
		while ((c = kbd_getc()) >= 0) {
			feed(c);
			audio_service();
		}
		while ((c = serial_getc()) >= 0) {
			feed(c);
			audio_service();
		}
		audio_service();
		__asm__ volatile ("pause");
	}
	tty_clear();
}

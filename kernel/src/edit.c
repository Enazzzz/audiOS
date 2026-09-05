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
static unsigned top_row;

/** Console width used for wrap (never 0). */
static unsigned wrap_cols(void)
{
	unsigned c = tty_cols();
	return c ? c : 80u;
}

/** Map a buffer offset to a wrapped (row, col). */
static void pos_to_vis(uint32_t pos, unsigned W, unsigned *row, unsigned *col)
{
	unsigned r = 0;
	unsigned c = 0;
	uint32_t n = pos < len ? pos : len;
	for (uint32_t i = 0; i < n; i++) {
		if (buf[i] == '\n') {
			r++;
			c = 0;
			continue;
		}
		c++;
		if (c >= W) {
			r++;
			c = 0;
		}
	}
	*row = r;
	*col = c;
}

/** Buffer offset of wrapped row `target_row`, column `target_col`. */
static uint32_t vis_to_pos(unsigned target_row, unsigned target_col, unsigned W)
{
	unsigned r = 0;
	unsigned c = 0;
	uint32_t i = 0;
	while (i < len) {
		if (r == target_row) {
			while (i < len && buf[i] != '\n' && c < target_col) {
				i++;
				c++;
				if (c >= W) {
					break;
				}
			}
			return i;
		}
		if (buf[i] == '\n') {
			r++;
			c = 0;
			i++;
			continue;
		}
		c++;
		i++;
		if (c >= W) {
			r++;
			c = 0;
		}
	}
	return len;
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

/**
 * Paint with wrap. Does not tty_clear — dirty cells only, so typing
 * does not flash the whole framebuffer.
 */
static void paint(void)
{
	unsigned cols = wrap_cols();
	unsigned rows = tty_rows();
	if (rows < 3) {
		rows = 3;
	}
	unsigned text_rows = rows - 2;
	unsigned crow = 0;
	unsigned ccol = 0;
	pos_to_vis(cur, cols, &crow, &ccol);
	if (crow < top_row) {
		top_row = crow;
	}
	if (crow >= top_row + text_rows) {
		top_row = crow - (text_rows - 1);
	}

	char head[160];
	ksnprintf(head, sizeof(head), "edit %s%s  ^O save  ^X quit", path, dirty ? "*" : "");
	for (unsigned c = 0; c < cols; c++) {
		char ch = (c < strlen(head)) ? head[c] : ' ';
		tty_put_xy(c, 0, ch, TTY_COL_ACCENT);
	}

	unsigned row = 0;
	unsigned col = 0;
	uint32_t i = 0;
	while (row < top_row + text_rows) {
		int on_screen = (row >= top_row);
		unsigned sr = 1 + (row - top_row);
		if (i >= len) {
			if (on_screen) {
				for (unsigned c = 0; c < cols; c++) {
					uint32_t rgb = (row == crow && c == ccol) ? TTY_COL_AUDIO : TTY_COL_FG;
					char ch = (row == crow && c == ccol) ? '_' : ' ';
					tty_put_xy(c, sr, ch, rgb);
				}
			}
			row++;
			continue;
		}
		/* Draw the wrapped row in one pass. Do not blank first —
		 * that flashes every keystroke. Unchanged cells are skipped. */
		col = 0;
		while (i < len && buf[i] != '\n' && col < cols) {
			char ch = buf[i];
			if (ch == '\t') {
				ch = ' ';
			}
			if (on_screen) {
				int here = (i == cur);
				tty_put_xy(col, sr, here ? (ch == ' ' ? '_' : ch) : ch,
					here ? TTY_COL_AUDIO : TTY_COL_FG);
			}
			i++;
			col++;
		}
		if (on_screen) {
			if (i == cur && col < cols && (i >= len || buf[i] == '\n')) {
				tty_put_xy(col, sr, '_', TTY_COL_AUDIO);
				col++;
			}
			while (col < cols) {
				tty_put_xy(col, sr, ' ', TTY_COL_FG);
				col++;
			}
		}
		if (i < len && buf[i] == '\n') {
			i++;
			row++;
			continue;
		}
		if (col >= cols) {
			row++;
			continue;
		}
		row++;
		if (i >= len) {
			/* fill remaining screen rows */
			continue;
		}
	}

	char st[80];
	ksnprintf(st, sizeof(st), "%u/%u bytes  wrap on", cur, len);
	for (unsigned c = 0; c < cols; c++) {
		char ch = (c < strlen(st)) ? st[c] : ' ';
		tty_put_xy(c, rows - 1, ch, TTY_COL_DIM);
	}
}

/** Move the cursor by `delta` wrapped rows, preserving column. */
static void move_vert(int delta)
{
	unsigned W = wrap_cols();
	unsigned r = 0;
	unsigned c = 0;
	pos_to_vis(cur, W, &r, &c);
	if (delta < 0 && r < (unsigned)(-delta)) {
		r = 0;
	} else {
		r = (unsigned)((int)r + delta);
	}
	cur = vis_to_pos(r, c, W);
}

/** Handle one key. */
static void feed(int c)
{
	unsigned W = wrap_cols();
	unsigned r = 0;
	unsigned col = 0;
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
		pos_to_vis(cur, W, &r, &col);
		cur = vis_to_pos(r, 0, W);
		paint();
		return;
	}
	if (c == KBD_END) {
		pos_to_vis(cur, W, &r, &col);
		cur = vis_to_pos(r, W - 1u, W);
		paint();
		return;
	}
	if (c == KBD_PGUP) {
		move_vert(-1);
		paint();
		return;
	}
	if (c == KBD_PGDN) {
		move_vert(1);
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
		tty_puts("usage: edit <file>   (^O save, ^X quit, wrap on)\n");
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
	top_row = 0;
	uint32_t n = 0;
	if (fs_read_file(path, buf, cap - 1u, &n) && n > 0) {
		len = n;
		if (len >= cap) {
			len = cap - 1;
		}
	}
	buf[len] = '\0';
	running = 1;
	kbd_flush_queue();
	tty_clear();
	tty_cursor_hide();
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

#include "shell.h"
#include "edit.h"
#include "tetris.h"
#include "audio.h"
#include "clip.h"
#include "cpu.h"
#include "fat.h"
#include "fs.h"
#include "help.h"
#include "kbd.h"
#include "klib.h"
#include "meminfo.h"
#include "pit.h"
#include "reboot.h"
#include "serial.h"
#include "tty.h"
#include "version.h"

#define LINE_MAX	256
#define ARG_MAX		24
#define HIST_MAX	32
#define SCRIPT_MAX	4096

static char line[LINE_MAX];
static unsigned line_len;
static unsigned line_cur;
static int sel_anchor;
static char clipbuf[LINE_MAX];
static unsigned prompt_col;
static unsigned prompt_row;
static unsigned vis_len;
static int hud_ok = 1;
static int esc_mod;
static char hist[HIST_MAX][LINE_MAX];
static unsigned hist_len;
static unsigned hist_pos;
static char draft[LINE_MAX];
static int browsing;
static int esc;	/* 0, 1 = ESC, 2 = CSI, 3 = SS3 (serial arrows) */
static unsigned esc_num;
static uint64_t scroll_hold_at;	/* PIT tick when hold-repeat may fire again */

/** Trim leading/trailing whitespace in place. */
static void trim_inplace(char *s)
{
	size_t n = strlen(s);
	while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\r' || s[n - 1] == '\n' || s[n - 1] == '\t')) {
		s[--n] = '\0';
	}
	char *p = s;
	while (*p == ' ' || *p == '\t') {
		p++;
	}
	if (p != s) {
		size_t rest = strlen(p);
		memmove(s, p, rest + 1);
	}
}

/** Split `s` into argv in place. */
static int split_args(char *s, char **argv, int max)
{
	int argc = 0;
	while (*s != '\0' && argc < max) {
		while (*s == ' ' || *s == '\t') {
			s++;
		}
		if (*s == '\0') {
			break;
		}
		argv[argc++] = s;
		while (*s != '\0' && *s != ' ' && *s != '\t') {
			s++;
		}
		if (*s != '\0') {
			*s++ = '\0';
		}
	}
	return argc;
}

/** Draw the identity prompt. */
static void shell_prompt(void)
{
	tty_set_color(TTY_COL_ACCENT);
	tty_puts("audiOS");
	tty_set_color(TTY_COL_FG);
	tty_puts("> ");
	prompt_col = tty_cursor_col();
	prompt_row = tty_cursor_row();
	line_cur = 0;
	sel_anchor = -1;
	vis_len = 0;
}

/** Print the boot banner that establishes the machine as an audio computer. */
static void shell_banner(void)
{
	tty_set_color(TTY_COL_FG);
	tty_puts(AUDIOS_BANNER "\n");
	tty_set_color(TTY_COL_AUDIO);
	tty_printf("%u kHz • %u-bit • %u channels\n",
		AUDIOS_AUDIO_RATE / 1000u, AUDIOS_AUDIO_BITS, AUDIOS_AUDIO_CHANNELS);
	if (tty_fb_width() && tty_fb_height()) {
		tty_set_color(TTY_COL_DIM);
		tty_printf("%ux%u framebuffer • %ux%u text\n",
			tty_fb_width(), tty_fb_height(), tty_cols(), tty_rows());
	}
	tty_set_color(TTY_COL_FG);
}

/** Built-in `help`. */
static void cmd_help(int argc, char **argv)
{
	if (argc > 1) {
		if (!help_topic(argv[1])) {
			tty_set_color(TTY_COL_ERR);
			tty_printf("no help topic '%s' — try help\n", argv[1]);
			tty_set_color(TTY_COL_FG);
		}
		return;
	}
	help_list();
}

/** Built-in `version`, including uptime from the system timer. */
static void cmd_version(void)
{
	uint64_t ticks = pit_ticks();
	uint64_t secs = ticks / PIT_HZ;
	uint64_t millis = (ticks % PIT_HZ);
	tty_puts(AUDIOS_BANNER "\n");
	tty_set_color(TTY_COL_DIM);
	tty_printf("build %u.%u.%u\n", AUDIOS_VERSION_MAJOR, AUDIOS_VERSION_MINOR, AUDIOS_VERSION_PATCH);
	tty_printf("board %s\n", AUDIOS_BOARD);
	tty_printf("uptime %llu.%03llu s\n",
		(unsigned long long)secs, (unsigned long long)millis);
	tty_printf("framebuffer %ux%u  text %ux%u\n",
		tty_fb_width(), tty_fb_height(), tty_cols(), tty_rows());
	tty_set_color(TTY_COL_FG);
}

/** Remember a non-empty command, like PowerShell history. */
static void hist_remember(const char *s)
{
	if (s[0] == '\0') {
		return;
	}
	if (hist_len > 0 && strcmp(hist[hist_len - 1], s) == 0) {
		browsing = 0;
		hist_pos = hist_len;
		return;
	}
	if (hist_len < HIST_MAX) {
		ksnprintf(hist[hist_len], LINE_MAX, "%s", s);
		hist_len++;
	} else {
		memmove(hist[0], hist[1], (HIST_MAX - 1) * LINE_MAX);
		ksnprintf(hist[HIST_MAX - 1], LINE_MAX, "%s", s);
	}
	browsing = 0;
	hist_pos = hist_len;
}

/** True if index `i` is inside the selection. */
static int in_sel(unsigned i)
{
	unsigned a, b;
	if (sel_anchor < 0) {
		return 0;
	}
	a = (unsigned)sel_anchor;
	b = line_cur;
	if (a > b) {
		unsigned t = a;
		a = b;
		b = t;
	}
	return i >= a && i < b;
}

/** Repaint the input line with selection highlight. */
static void shell_repaint(void)
{
	unsigned cols = tty_cols();
	unsigned i;
	unsigned lim = vis_len;
	if (line_len + 1u > lim) {
		lim = line_len + 1u;
	}
	tty_cursor_hide();
	for (i = 0; i < lim && i < LINE_MAX - 1u; i++) {
		unsigned col = (prompt_col + i) % (cols ? cols : 1);
		unsigned row = prompt_row + (prompt_col + i) / (cols ? cols : 1);
		char ch = (i < line_len) ? line[i] : ' ';
		if (ch == 0) {
			ch = ' ';
		}
		if (in_sel(i) && i < line_len) {
			tty_put_xy_bg(col, row, ch, TTY_COL_SEL_FG, TTY_COL_SEL_BG);
		} else {
			tty_put_xy(col, row, ch, TTY_COL_FG);
		}
	}
	vis_len = line_len;
	{
		unsigned col = (prompt_col + line_cur) % (cols ? cols : 1);
		unsigned row = prompt_row + (prompt_col + line_cur) / (cols ? cols : 1);
		tty_set_cursor(col, row);
	}
}

/** Replace the in-progress line on screen. */
static void shell_paint_line(const char *s)
{
	ksnprintf(line, LINE_MAX, "%s", s);
	line_len = (unsigned)strlen(line);
	line_cur = line_len;
	sel_anchor = -1;
	shell_repaint();
}

static void sel_clear(void)
{
	sel_anchor = -1;
}

static void sel_ensure(int shift)
{
	if (shift) {
		if (sel_anchor < 0) {
			sel_anchor = (int)line_cur;
		}
	} else {
		sel_anchor = -1;
	}
}

static void sel_delete(void)
{
	unsigned a, b, n;
	if (sel_anchor < 0) {
		return;
	}
	a = (unsigned)sel_anchor;
	b = line_cur;
	if (a > b) {
		unsigned t = a;
		a = b;
		b = t;
	}
	n = b - a;
	if (n == 0) {
		sel_anchor = -1;
		return;
	}
	memmove(line + a, line + b, line_len - b + 1u);
	line_len -= n;
	line_cur = a;
	sel_anchor = -1;
}

static void copy_sel(void)
{
	unsigned a, b, n;
	if (sel_anchor < 0) {
		return;
	}
	a = (unsigned)sel_anchor;
	b = line_cur;
	if (a > b) {
		unsigned t = a;
		a = b;
		b = t;
	}
	n = b - a;
	if (n >= LINE_MAX) {
		n = LINE_MAX - 1u;
	}
	memcpy(clipbuf, line + a, n);
	clipbuf[n] = '\0';
}

static void paste_clip(void)
{
	unsigned n = (unsigned)strlen(clipbuf);
	unsigned i;
	sel_delete();
	if (line_len + n >= LINE_MAX) {
		n = LINE_MAX - 1u - line_len;
	}
	memmove(line + line_cur + n, line + line_cur, line_len - line_cur + 1u);
	for (i = 0; i < n; i++) {
		line[line_cur + i] = clipbuf[i];
	}
	line_len += n;
	line_cur += n;
}

static void delete_word(void)
{
	unsigned i;
	sel_delete();
	if (line_cur == 0) {
		return;
	}
	i = line_cur;
	while (i > 0 && line[i - 1u] == ' ') {
		i--;
	}
	while (i > 0 && line[i - 1u] != ' ') {
		i--;
	}
	memmove(line + i, line + line_cur, line_len - line_cur + 1u);
	line_len -= (line_cur - i);
	line_cur = i;
}

/** Previous command (PowerShell UpArrow). */
static void hist_up(void)
{
	if (hist_len == 0) {
		return;
	}
	if (!browsing) {
		line[line_len] = '\0';
		ksnprintf(draft, sizeof(draft), "%s", line);
		browsing = 1;
		hist_pos = hist_len;
	}
	if (hist_pos == 0) {
		return;
	}
	hist_pos--;
	shell_paint_line(hist[hist_pos]);
}

/** Next command, or the draft line at the end of history. */
static void hist_down(void)
{
	if (!browsing) {
		return;
	}
	if (hist_pos + 1u < hist_len) {
		hist_pos++;
		shell_paint_line(hist[hist_pos]);
		return;
	}
	browsing = 0;
	hist_pos = hist_len;
	shell_paint_line(draft);
}

/** Dispatch a complete command line. Empty lines are ignored. */
static void shell_dispatch(char *cmd);

/**
 * Easter egg. Classic 7-bit ASCII cat (ASCII Art Archive / asciiart.eu,
 * “Cat by unknown”, 2014). Not listed in help; `type` dumps files.
 */
static void cmd_cat_art(void)
{
	tty_set_color(TTY_COL_ACCENT);
	tty_puts("  /\\_/\\\n");
	tty_puts(" ( o.o )\n");
	tty_puts("  > ^ <\n");
	tty_set_color(TTY_COL_FG);
}

/** Run each non-comment line of a text file as a command. */
static void cmd_script(const char *path)
{
	static int depth;
	static char buf[SCRIPT_MAX];
	if (path == NULL || path[0] == '\0') {
		tty_set_color(TTY_COL_ERR);
		tty_puts("usage: script <file>\n");
		tty_set_color(TTY_COL_FG);
		return;
	}
	if (depth > 4) {
		tty_set_color(TTY_COL_ERR);
		tty_puts("script: too much nesting\n");
		tty_set_color(TTY_COL_FG);
		return;
	}
	uint32_t n = 0;
	if (!fs_read_file(path, buf, SCRIPT_MAX - 1u, &n) || n == 0) {
		tty_set_color(TTY_COL_ERR);
		tty_printf("script: %s\n", fs_error()[0] ? fs_error() : "file not found");
		tty_set_color(TTY_COL_FG);
		return;
	}
	buf[n] = '\0';
	{
		char *start = buf;
		if (n >= 5 && start[0] == 'A' && start[1] == 'O' && start[2] == 'S'
			&& start[3] == '1' && (start[4] == '\n' || start[4] == '\r')) {
			start += 5;
			if (*start == '\n') {
				start++;
			}
		}
		depth++;
		char *p = start;
	while (*p) {
		char *line = p;
		while (*p && *p != '\n' && *p != '\r') {
			p++;
		}
		char save = *p;
		*p = '\0';
		char *s = line;
		while (*s == ' ' || *s == '\t') {
			s++;
		}
		if (*s != '\0' && *s != '#') {
			char tmp[LINE_MAX];
			ksnprintf(tmp, sizeof(tmp), "%s", s);
			shell_dispatch(tmp);
		}
		*p = save;
		if (*p == '\r') {
			p++;
		}
		if (*p == '\n') {
			p++;
		}
	}
		depth--;
	}
}

#define SESSION_PATH	"D:/session.aos"
#define AUTOEXEC_D	"D:/autoexec.aos"
#define AUTOEXEC_C	"C:/autoexec.aos"

/** Persist cwd / clip / mixer / seq clock onto D: if it is mounted. */
static void session_save(void)
{
	char body[256];
	char cwd[FAT_PATH_MAX];
	if (!fat_vol_ready(FAT_VOL_USR)) {
		return;
	}
	fs_getcwd(cwd, sizeof(cwd));
	ksnprintf(body, sizeof(body),
		"AOS1\ncwd %s\nclip %s\nvol %u\nlimiter %u\ningain %u\nbpm %u\n",
		cwd,
		clip_current_name()[0] ? clip_current_name() : "-",
		audio_volume(),
		(unsigned)audio_limiter(),
		audio_ingain(),
		seq_get_bpm());
	(void)fs_write_file(SESSION_PATH, body, (uint32_t)strlen(body));
}

/** Apply key/value from a session file. */
static void session_apply_line(char *s)
{
	char *argv[8];
	int n;
	while (*s == ' ' || *s == '\t') {
		s++;
	}
	if (*s == '\0' || *s == '#') {
		return;
	}
	n = split_args(s, argv, 8);
	if (n < 1) {
		return;
	}
	if (strcmp(argv[0], "cwd") == 0 && n > 1) {
		(void)fs_chdir(argv[1]);
	} else if (strcmp(argv[0], "clip") == 0 && n > 1 && argv[1][0] != '-') {
		if (clip_find(argv[1]) != NULL) {
			clip_use(argv[1]);
		}
	} else if (strcmp(argv[0], "vol") == 0 && n > 1) {
		unsigned v = 0;
		const char *p = argv[1];
		while (*p >= '0' && *p <= '9') {
			v = v * 10u + (unsigned)(*p - '0');
			p++;
		}
		audio_set_volume(v);
	} else if (strcmp(argv[0], "limiter") == 0 && n > 1) {
		audio_set_limiter(argv[1][0] != '0');
	} else if (strcmp(argv[0], "ingain") == 0 && n > 1) {
		unsigned v = 0;
		const char *p = argv[1];
		while (*p >= '0' && *p <= '9') {
			v = v * 10u + (unsigned)(*p - '0');
			p++;
		}
		audio_set_ingain(v);
	} else if (strcmp(argv[0], "bpm") == 0 && n > 1) {
		unsigned v = 0;
		const char *p = argv[1];
		while (*p >= '0' && *p <= '9') {
			v = v * 10u + (unsigned)(*p - '0');
			p++;
		}
		seq_set_bpm(v);
	}
}

static void session_load(void)
{
	char buf[512];
	uint32_t n = 0;
	char *p;
	if (!fs_read_file(SESSION_PATH, buf, sizeof(buf) - 1u, &n) || n == 0) {
		return;
	}
	buf[n] = '\0';
	p = buf;
	if (n >= 5 && p[0] == 'A' && p[1] == 'O' && p[2] == 'S' && p[3] == '1') {
		while (*p && *p != '\n') {
			p++;
		}
		if (*p == '\n') {
			p++;
		}
	}
	while (*p) {
		char *line = p;
		while (*p && *p != '\n' && *p != '\r') {
			p++;
		}
		char save = *p;
		*p = '\0';
		session_apply_line(line);
		*p = save;
		if (*p == '\r') {
			p++;
		}
		if (*p == '\n') {
			p++;
		}
	}
}

/** Short y/n with a 2 s default-n so QEMU tests are not stuck. */
static void session_boot_prompt(void)
{
	uint32_t n = 0;
	char probe[8];
	uint64_t until;
	if (!fat_vol_ready(FAT_VOL_USR)) {
		return;
	}
	if (!fs_read_file(SESSION_PATH, probe, sizeof(probe) - 1u, &n) || n < 4) {
		return;
	}
	tty_puts("Restore last session? y/n (2s = n) ");
	until = pit_ticks() + 2000u;
	for (;;) {
		int c = kbd_getc();
		int s = serial_getc();
		if (c < 0) {
			c = s;
		}
		if (c == 'y' || c == 'Y') {
			tty_puts("y\n");
			session_load();
			return;
		}
		if (c == 'n' || c == 'N') {
			tty_puts("n\n");
			return;
		}
		if (pit_ticks() >= until) {
			tty_puts("n\n");
			return;
		}
		audio_service();
		__asm__ volatile ("pause");
	}
}

static int aos_has_magic(const char *buf, uint32_t n)
{
	return n >= 5 && buf[0] == 'A' && buf[1] == 'O' && buf[2] == 'S'
		&& buf[3] == '1' && (buf[4] == '\n' || buf[4] == '\r');
}

/** Run D:/autoexec.aos or C:/autoexec.aos when it has an AOS1 header. */
static void run_autoexec(void)
{
	char buf[8];
	uint32_t n = 0;
	const char *path = AUTOEXEC_D;
	if (!fs_read_file(path, buf, sizeof(buf) - 1u, &n) || !aos_has_magic(buf, n)) {
		n = 0;
		path = AUTOEXEC_C;
		if (!fs_read_file(path, buf, sizeof(buf) - 1u, &n) || !aos_has_magic(buf, n)) {
			return;
		}
	}
	tty_printf("autoexec %s\n", path);
	cmd_script(path);
}

static void shell_dispatch(char *cmd)
{
	trim_inplace(cmd);
	if (cmd[0] == '\0') {
		return;
	}
	char *argv[ARG_MAX];
	int argc = split_args(cmd, argv, ARG_MAX);
	if (argc == 0) {
		return;
	}
	if (strcmp(argv[0], "help") == 0) {
		cmd_help(argc, argv);
	} else if (strcmp(argv[0], "clear") == 0) {
		tty_clear();
		shell_banner();
	} else if (strcmp(argv[0], "version") == 0) {
		cmd_version();
	} else if (strcmp(argv[0], "cpu") == 0) {
		cpu_print();
	} else if (strcmp(argv[0], "mem") == 0) {
		meminfo_print();
	} else if (strcmp(argv[0], "audio") == 0) {
		audio_cmd(argc, argv);
	} else if (strcmp(argv[0], "tone") == 0) {
		tone_cmd(argc, argv);
	} else if (strcmp(argv[0], "play") == 0) {
		play_cmd(argc, argv);
	} else if (strcmp(argv[0], "stop") == 0) {
		stop_cmd();
	} else if (strcmp(argv[0], "ls") == 0) {
		fs_cmd_ls(argc, argv);
	} else if (strcmp(argv[0], "cd") == 0) {
		fs_cmd_cd(argc, argv);
	} else if (strcmp(argv[0], "pwd") == 0) {
		fs_cmd_pwd();
	} else if (strcmp(argv[0], "mkdir") == 0) {
		fs_cmd_mkdir(argc, argv);
	} else if (strcmp(argv[0], "rm") == 0) {
		fs_cmd_rm(argc, argv);
	} else if (strcmp(argv[0], "cp") == 0) {
		fs_cmd_cp(argc, argv);
	} else if (strcmp(argv[0], "mv") == 0) {
		fs_cmd_mv(argc, argv);
	} else if (strcmp(argv[0], "type") == 0) {
		fs_cmd_type(argc, argv);
	} else if (strcmp(argv[0], "cat") == 0) {
		cmd_cat_art();
	} else if (strcmp(argv[0], "touch") == 0) {
		fs_cmd_touch(argc, argv);
	} else if (strcmp(argv[0], "info") == 0) {
		fs_cmd_info(argc, argv);
	} else if (strcmp(argv[0], "storage") == 0) {
		fs_cmd_storage();
	} else if (strcmp(argv[0], "drives") == 0) {
		fs_cmd_drives();
	} else if (strcmp(argv[0], "update") == 0) {
		fs_cmd_update(argc, argv);
	} else if (strcmp(argv[0], "script") == 0) {
		cmd_script(argc > 1 ? argv[1] : "");
	} else if (strcmp(argv[0], "mount") == 0) {
		fs_cmd_mount();
	} else if (strcmp(argv[0], "edit") == 0) {
		edit_cmd(argc, argv);
	} else if (strcmp(argv[0], "tetris") == 0) {
		tetris_cmd(argc, argv);
	} else if (strcmp(argv[0], "reboot") == 0) {
		session_save();
		system_reboot();
	} else if (strcmp(argv[0], "shutdown") == 0) {
		session_save();
		system_shutdown();
	} else if (fs_is_drive(argv[0]) || (argv[0][0] && argv[0][1] == ':')) {
		fs_cmd_cd(argc, argv);
	} else if (music_is_verb(argv[0])) {
		music_cmd(argc, argv);
	} else {
		tty_set_color(TTY_COL_ERR);
		tty_puts("no such command -- try help\n");
		tty_set_color(TTY_COL_FG);
	}
}

/** Feed one input character into the line editor. */
static void shell_feed(int c)
{
	int shift = kbd_shift();
	if (c == KBD_F5) {
		if (audio_is_playing()) {
			audio_pause_toggle();
			tty_puts(audio_paused() ? "\npaused\n" : "\nresumed\n");
			shell_prompt();
			shell_repaint();
		} else if (clip_current() != NULL) {
			char tmp[LINE_MAX];
			ksnprintf(tmp, sizeof(tmp), "play %s", clip_current_name());
			tty_putc('\n');
			shell_dispatch(tmp);
			shell_prompt();
			shell_repaint();
		}
		return;
	}
	if (c == KBD_F6) {
		tty_putc('\n');
		stop_cmd();
		shell_prompt();
		shell_repaint();
		return;
	}
	if (c == KBD_F11) {
		audio_bump_volume(-5);
		return;
	}
	if (c == KBD_F12) {
		audio_bump_volume(5);
		return;
	}
	if (c == 0x1B) {
		esc = 1;
		esc_num = 0;
		esc_mod = 0;
		return;
	}
	if (esc == 1) {
		if (c == '[') {
			esc = 2;
			esc_num = 0;
			esc_mod = 0;
			return;
		}
		if (c == 'O') {
			esc = 3;
			return;
		}
		esc = 0;
	} else if (esc == 2 || esc == 3 || esc == 4) {
		if ((esc == 2 || esc == 4) && c >= '0' && c <= '9') {
			if (esc == 4) {
				esc_mod = esc_mod * 10 + (int)(c - '0');
			} else {
				esc_num = esc_num * 10u + (unsigned)(c - '0');
			}
			return;
		}
		if (esc == 2 && c == ';') {
			esc = 4;
			esc_mod = 0;
			return;
		}
		esc = 0;
		if (esc_mod == 1) {
			esc_mod = 0;
		}
		shift = shift || (esc_mod == 2 || esc_mod == 6);
		if (c == 'A') {
			hist_up();
			return;
		}
		if (c == 'B') {
			hist_down();
			return;
		}
		if (c == 'C' || c == KBD_RIGHT) {
			sel_ensure(shift);
			if (line_cur < line_len) {
				line_cur++;
			}
			shell_repaint();
			return;
		}
		if (c == 'D' || c == KBD_LEFT) {
			sel_ensure(shift);
			if (line_cur > 0) {
				line_cur--;
			}
			shell_repaint();
			return;
		}
		if (c == 'H') {
			sel_ensure(shift);
			line_cur = 0;
			shell_repaint();
			return;
		}
		if (c == 'F') {
			sel_ensure(shift);
			line_cur = line_len;
			shell_repaint();
			return;
		}
		if (c == '~') {
			if (esc_num == 5) {
				tty_page_up();
			} else if (esc_num == 6) {
				tty_page_down();
			} else if (esc_num == 15) {
				shell_feed(KBD_F5);
			} else if (esc_num == 17) {
				shell_feed(KBD_F6);
			} else if (esc_num == 23) {
				shell_feed(KBD_F11);
			} else if (esc_num == 24) {
				shell_feed(KBD_F12);
			}
			return;
		}
		return;
	}
	if (c == KBD_LEFT) {
		sel_ensure(shift);
		if (line_cur > 0) {
			line_cur--;
		}
		shell_repaint();
		return;
	}
	if (c == KBD_RIGHT) {
		sel_ensure(shift);
		if (line_cur < line_len) {
			line_cur++;
		}
		shell_repaint();
		return;
	}
	if (c == KBD_HOME) {
		sel_ensure(shift);
		line_cur = 0;
		shell_repaint();
		return;
	}
	if (c == KBD_END) {
		sel_ensure(shift);
		line_cur = line_len;
		shell_repaint();
		return;
	}
	if (c == '\r') {
		c = '\n';
	}
	if (c == '\n') {
		sel_clear();
		tty_putc('\n');
		line[line_len] = '\0';
		hist_remember(line);
		shell_dispatch(line);
		line_len = 0;
		line_cur = 0;
		vis_len = 0;
		browsing = 0;
		shell_prompt();
		return;
	}
	if (c == KBD_UP) {
		hist_up();
		return;
	}
	if (c == KBD_DOWN) {
		hist_down();
		return;
	}
	if (c == KBD_PGUP) {
		tty_page_up();
		scroll_hold_at = pit_ticks() + 90u;
		return;
	}
	if (c == KBD_PGDN) {
		tty_page_down();
		scroll_hold_at = pit_ticks() + 90u;
		return;
	}
	if (c == 3) {
		/* Ctrl-C: copy selection, or cancel the line. */
		if (sel_anchor >= 0) {
			copy_sel();
			return;
		}
		line_len = 0;
		line_cur = 0;
		sel_clear();
		tty_putc('\n');
		shell_prompt();
		return;
	}
	if (c == 22) {
		paste_clip();
		shell_repaint();
		return;
	}
	if (c == KBD_C_BS || c == 0x17) {
		/* Ctrl-Backspace (PS/2) or Ctrl-W (serial). */
		delete_word();
		shell_repaint();
		return;
	}
	if (c == '\b' || c == 0x7F) {
		if (sel_anchor >= 0) {
			sel_delete();
		} else if (line_cur > 0) {
			memmove(line + line_cur - 1u, line + line_cur, line_len - line_cur + 1u);
			line_cur--;
			line_len--;
		}
		shell_repaint();
		return;
	}
	if (c < 32 || c > 126) {
		return;
	}
	if (sel_anchor >= 0) {
		sel_delete();
	}
	if (line_len + 1 >= LINE_MAX) {
		return;
	}
	memmove(line + line_cur + 1u, line + line_cur, line_len - line_cur + 1u);
	line[line_cur] = (char)c;
	line_cur++;
	line_len++;
	line[line_len] = '\0';
	shell_repaint();
}

/** Print the identity banner and run the command loop. */
void shell_run(void)
{
	line_len = 0;
	line_cur = 0;
	sel_anchor = -1;
	clipbuf[0] = '\0';
	hist_len = 0;
	hist_pos = 0;
	browsing = 0;
	esc = 0;
	esc_mod = 0;
	scroll_hold_at = 0;
	hud_ok = 1;
	tty_set_idle(audio_service);
	shell_banner();
	session_boot_prompt();
	run_autoexec();
	shell_prompt();
	for (;;) {
		int c;
		while ((c = kbd_getc()) >= 0) {
			shell_feed(c);
			audio_service();
		}
		while ((c = serial_getc()) >= 0) {
			shell_feed(c);
			audio_service();
		}
		uint64_t now = pit_ticks();
		if (now >= scroll_hold_at) {
			if (kbd_held(KBD_PGUP)) {
				tty_line_up();
				scroll_hold_at = now + 90u;
			} else if (kbd_held(KBD_PGDN)) {
				tty_line_down();
				scroll_hold_at = now + 90u;
			}
		}
		audio_service();
		if (hud_ok) {
			audio_draw_hud();
		}
		tty_tick_cursor(pit_ticks());
		__asm__ volatile ("pause");
	}
}

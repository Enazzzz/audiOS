#include "shell.h"
#include "audio.h"
#include "clip.h"
#include "cpu.h"
#include "fs.h"
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
static char hist[HIST_MAX][LINE_MAX];
static unsigned hist_len;
static unsigned hist_pos;
static char draft[LINE_MAX];
static int browsing;
static int esc;	/* 0, 1 = ESC, 2 = CSI, 3 = SS3 (serial arrows) */

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
}

/** Print the boot banner that establishes the machine as an audio computer. */
static void shell_banner(void)
{
	tty_set_color(TTY_COL_FG);
	tty_puts(AUDIOS_BANNER "\n");
	tty_set_color(TTY_COL_AUDIO);
	tty_printf("%u kHz • %u-bit • %u channels\n",
		AUDIOS_AUDIO_RATE / 1000u, AUDIOS_AUDIO_BITS, AUDIOS_AUDIO_CHANNELS);
	tty_set_color(TTY_COL_FG);
}

/** Built-in `help`. */
static void cmd_help(void)
{
	tty_puts("audiOS commands\n");
	tty_set_color(TTY_COL_DIM);
	tty_puts("  help              this list\n");
	tty_puts("  clear             clear the console\n");
	tty_puts("  version           system version\n");
	tty_puts("  cpu               processor\n");
	tty_puts("  mem               physical memory\n");
	tty_puts("  audio             audio configuration\n");
	tty_puts("  audio devices     list output devices\n");
	tty_puts("  audio info        selected device details\n");
	tty_puts("  audio set         change rate/buffer/format\n");
	tty_puts("  audio status      runtime statistics\n");
	tty_puts("  audio test        continuous test tone\n");
	tty_puts("  tone              sine/square/saw/noise (no duration = until stop)\n");
	tty_puts("  play <clip|file>  play a clip or PCM WAV (loop|n)\n");
	tty_puts("  stop              halt playback\n");
	tty_puts("  music             clip / DSP / seq / rec commands\n");
	tty_puts("  script <file>     run commands from a text file\n");
	tty_puts("  ls [path]         list directory\n");
	tty_puts("  cd [path]         change directory\n");
	tty_puts("  pwd               print working directory\n");
	tty_puts("  mkdir <dir>       create directory\n");
	tty_puts("  rm <path>         delete file or empty directory\n");
	tty_puts("  cp <src> <dst>    copy file\n");
	tty_puts("  mv <src> <dst>    rename or move file\n");
	tty_puts("  cat <file>        show file bytes\n");
	tty_puts("  touch <file>      create empty file\n");
	tty_puts("  info <path>       file metadata\n");
	tty_puts("  storage           capacity and free space\n");
	tty_puts("  mount             mount status / retry\n");
	tty_puts("  reboot            restart the machine\n");
	tty_puts("  Up / Down         previous / next command (PowerShell-style)\n");
	tty_set_color(TTY_COL_FG);
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

/** Replace the in-progress line on screen. */
static void shell_paint_line(const char *s)
{
	while (line_len > 0) {
		tty_putc('\b');
		line_len--;
	}
	line_len = 0;
	while (s[line_len] != '\0' && line_len + 1u < LINE_MAX) {
		line[line_len] = s[line_len];
		tty_putc(s[line_len]);
		line_len++;
	}
	line[line_len] = '\0';
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
	depth++;
	char *p = buf;
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
		cmd_help();
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
	} else if (strcmp(argv[0], "cat") == 0) {
		fs_cmd_cat(argc, argv);
	} else if (strcmp(argv[0], "touch") == 0) {
		fs_cmd_touch(argc, argv);
	} else if (strcmp(argv[0], "info") == 0) {
		fs_cmd_info(argc, argv);
	} else if (strcmp(argv[0], "storage") == 0) {
		fs_cmd_storage();
	} else if (strcmp(argv[0], "script") == 0) {
		cmd_script(argc > 1 ? argv[1] : "");
	} else if (strcmp(argv[0], "mount") == 0) {
		fs_cmd_mount();
	} else if (strcmp(argv[0], "reboot") == 0) {
		system_reboot();
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
	/* Serial terminals send CSI/SS3 for arrows; the PS/2 path uses KBD_UP. */
	if (c == 0x1B) {
		esc = 1;
		return;
	}
	if (esc == 1) {
		if (c == '[') {
			esc = 2;
			return;
		}
		if (c == 'O') {
			esc = 3;
			return;
		}
		esc = 0;
	} else if (esc == 2 || esc == 3) {
		esc = 0;
		if (c == 'A') {
			hist_up();
			return;
		}
		if (c == 'B') {
			hist_down();
			return;
		}
		return;
	}
	if (c == '\r') {
		c = '\n';
	}
	if (c == '\n') {
		tty_putc('\n');
		line[line_len] = '\0';
		hist_remember(line);
		shell_dispatch(line);
		line_len = 0;
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
	if (c == '\b' || c == 0x7F) {
		if (line_len > 0) {
			line_len--;
			tty_putc('\b');
		}
		return;
	}
	if (c < 32 || c > 126) {
		return;
	}
	if (line_len + 1 >= LINE_MAX) {
		return;
	}
	line[line_len++] = (char)c;
	tty_putc((char)c);
}

/** Print the identity banner and run the command loop. */
void shell_run(void)
{
	line_len = 0;
	hist_len = 0;
	hist_pos = 0;
	browsing = 0;
	esc = 0;
	tty_set_idle(audio_service);
	shell_banner();
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
		audio_service();
		tty_tick_cursor(pit_ticks());
		__asm__ volatile ("pause");
	}
}

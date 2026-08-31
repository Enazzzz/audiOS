#include "shell.h"
#include "audio.h"
#include "cpu.h"
#include "kbd.h"
#include "klib.h"
#include "meminfo.h"
#include "pit.h"
#include "reboot.h"
#include "serial.h"
#include "tty.h"
#include "version.h"

#define LINE_MAX	96

static char line[LINE_MAX];
static unsigned line_len;

/** Trim trailing CR/LF/spaces in place. */
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
	tty_puts("  help      this list\n");
	tty_puts("  clear     clear the console\n");
	tty_puts("  version   system version\n");
	tty_puts("  cpu       processor\n");
	tty_puts("  mem       physical memory\n");
	tty_puts("  audio     audio subsystem\n");
	tty_puts("  reboot    restart the machine\n");
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
	tty_printf("uptime %llu.%03llu s\n",
		(unsigned long long)secs, (unsigned long long)millis);
	tty_set_color(TTY_COL_FG);
}

/** Dispatch a complete command line. Empty lines are ignored. */
static void shell_dispatch(char *cmd)
{
	trim_inplace(cmd);
	if (cmd[0] == '\0') {
		return;
	}
	if (strcmp(cmd, "help") == 0) {
		cmd_help();
	} else if (strcmp(cmd, "clear") == 0) {
		tty_clear();
		shell_banner();
	} else if (strcmp(cmd, "version") == 0) {
		cmd_version();
	} else if (strcmp(cmd, "cpu") == 0) {
		cpu_print();
	} else if (strcmp(cmd, "mem") == 0) {
		meminfo_print();
	} else if (strcmp(cmd, "audio") == 0) {
		audio_print();
	} else if (strcmp(cmd, "reboot") == 0) {
		system_reboot();
	} else {
		tty_set_color(TTY_COL_ERR);
		tty_puts("no such command -- try help\n");
		tty_set_color(TTY_COL_FG);
	}
}

/** Feed one input character into the line editor. */
static void shell_feed(int c)
{
	if (c == '\r') {
		c = '\n';
	}
	if (c == '\n') {
		tty_putc('\n');
		line[line_len] = '\0';
		shell_dispatch(line);
		line_len = 0;
		shell_prompt();
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
	shell_banner();
	shell_prompt();
	for (;;) {
		int c;
		while ((c = kbd_getc()) >= 0) {
			shell_feed(c);
		}
		while ((c = serial_getc()) >= 0) {
			shell_feed(c);
		}
		tty_tick_cursor(pit_ticks());
		__asm__ volatile ("pause");
	}
}

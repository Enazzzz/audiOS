#include "alink.h"
#include "audio.h"
#include "cpu.h"
#include "idt.h"
#include "io.h"
#include "kbd.h"
#include "klib.h"
#include "pci.h"
#include "pic.h"
#include "pit.h"
#include "serial.h"
#include "tty.h"
#include "version.h"

#include <stdint.h>

static char line[120];
static unsigned llen;

static void reboot(void)
{
	tty_puts("rebooting...\n");
	__asm__ volatile ("cli");
	outb(0x64, 0xFE);
	for (;;) {
		__asm__ volatile ("hlt");
	}
}

/** Split `s` into argv (destructive). */
static int split(char *s, char **argv, int max)
{
	int n = 0;
	while (*s && n < max) {
		while (*s == ' ') {
			s++;
		}
		if (!*s) {
			break;
		}
		argv[n++] = s;
		while (*s && *s != ' ') {
			s++;
		}
		if (*s) {
			*s++ = '\0';
		}
	}
	return n;
}

static void run_line(const char *raw);

static void dispatch(int argc, char **argv)
{
	if (argc < 1) {
		return;
	}
	if (strcmp(argv[0], "help") == 0) {
		tty_puts("audiOS slave — 32-bit A7V333\n");
		tty_puts("help cpu link reboot version\n");
		return;
	}
	if (strcmp(argv[0], "cpu") == 0) {
		cpu_print();
		return;
	}
	if (strcmp(argv[0], "link") == 0) {
		alink_cmd(argc, argv);
		return;
	}
	if (strcmp(argv[0], "reboot") == 0) {
		reboot();
		return;
	}
	if (strcmp(argv[0], "version") == 0) {
		tty_printf("%s  i386 slave  %s\n", AUDIOS_SLAVE_BANNER, AUDIOS_SLAVE_BOARD);
		return;
	}
	tty_set_color(TTY_COL_ERR);
	tty_puts("no such command\n");
	tty_set_color(TTY_COL_FG);
}

static void run_line(const char *raw)
{
	char tmp[120];
	char *argv[8];
	int argc;
	ksnprintf(tmp, sizeof(tmp), "%s", raw);
	argc = split(tmp, argv, 8);
	dispatch(argc, argv);
}

static void prompt(void)
{
	tty_set_color(TTY_COL_ACCENT);
	tty_puts("slave> ");
	tty_set_color(TTY_COL_FG);
}

static void feed(int c)
{
	if (alink_viewing()) {
		if (c == 0x18) {
			alink_exit_view();
			tty_puts("\nlocal\n");
			prompt();
			return;
		}
		alink_send_key(c);
		return;
	}
	if (c == '\n' || c == '\r') {
		tty_putc('\n');
		line[llen] = '\0';
		if (llen) {
			run_line(line);
		}
		llen = 0;
		prompt();
		return;
	}
	if (c == '\b' || c == 127) {
		if (llen) {
			llen--;
			tty_puts("\b \b");
		}
		return;
	}
	if (c >= 32 && c < 127 && llen + 1u < sizeof(line)) {
		line[llen++] = (char)c;
		tty_putc((char)c);
	}
}

void kmain(void)
{
	unsigned i;
	tty_init();
	pic_init();
	idt_init();
	pit_init();
	kbd_init();
	__asm__ volatile ("sti");
	pci_init();
	audio_init();
	alink_init();
	alink_set_runner(run_line);

	tty_set_color(TTY_COL_AUDIO);
	tty_puts(AUDIOS_SLAVE_BANNER);
	tty_putc('\n');
	tty_set_color(TTY_COL_FG);
	tty_puts("32-bit  i686  ");
	tty_puts(AUDIOS_SLAVE_BOARD);
	tty_puts("\nPulse PHY  48 kHz stereo 12-bit PAM  ~976 kbit/s  role SLAVE\n");
	tty_puts("boot: IDE HDD or 1.44 MB floppy\n");
	tty_printf("codec: %s\n", audio_name());
	tty_printf("pci audio functions: %u\n", pci_device_count());
	for (i = 0; i < pci_device_count(); i++) {
		const struct pci_device *d = pci_device_at(i);
		tty_printf("  %u:%02x.%u  %04x:%04x\n",
			d->bus, d->slot, d->func, d->vendor, d->device);
	}

	if (alink_start_slave()) {
		tty_puts("link listening as SLAVE (Ctrl-X leaves view)\n");
	}

	prompt();
	for (;;) {
		int c;
		while ((c = kbd_getc()) >= 0) {
			feed(c);
		}
		while ((c = serial_getc()) >= 0) {
			feed(c);
		}
		alink_poll();
		alink_service();
		__asm__ volatile ("pause");
	}
}

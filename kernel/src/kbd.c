#include "kbd.h"
#include "io.h"
#include "pit.h"

#include <stdint.h>

#define KBD_DATA	0x60
#define KBD_STATUS	0x64
#define KBD_BUF_SIZE	128

#define ST_OUT		0x01
#define ST_IN		0x02

/* 8042 controller commands. */
#define CC_READ_CFG	0x20
#define CC_WRITE_CFG	0x60
#define CC_DISABLE_AUX	0xA7
#define CC_DISABLE_KBD	0xAD
#define CC_ENABLE_KBD	0xAE

/* Keyboard device commands. */
#define KD_RESET	0xFF
#define KD_ENABLE	0xF4
#define KD_ACK		0xFA
#define KD_BAT_OK	0xAA

/*
 * Config byte: IRQ1 on, keyboard clock on, scancode-set-1 translation on.
 * USB HDD boot plus EHCI handoff often clears these; ISO/CD boot does not.
 */
#define CFG_INT1	0x01
#define CFG_SYS		0x04
#define CFG_DISABLE_KBD	0x10
#define CFG_XLAT	0x40

static int queue[KBD_BUF_SIZE];
static unsigned qhead;
static unsigned qtail;
static int shift;
static int ctrl;
static int extended;
static int held_left;
static int held_right;
static int held_down;
static int held_up;
static int held_pgup;
static int held_pgdn;

static const char map_unshifted[128] = {
	[0x02] = '1', [0x03] = '2', [0x04] = '3', [0x05] = '4', [0x06] = '5',
	[0x07] = '6', [0x08] = '7', [0x09] = '8', [0x0A] = '9', [0x0B] = '0',
	[0x0C] = '-', [0x0D] = '=', [0x0E] = '\b', [0x0F] = '\t',
	[0x10] = 'q', [0x11] = 'w', [0x12] = 'e', [0x13] = 'r', [0x14] = 't',
	[0x15] = 'y', [0x16] = 'u', [0x17] = 'i', [0x18] = 'o', [0x19] = 'p',
	[0x1A] = '[', [0x1B] = ']', [0x1C] = '\n',
	[0x1E] = 'a', [0x1F] = 's', [0x20] = 'd', [0x21] = 'f', [0x22] = 'g',
	[0x23] = 'h', [0x24] = 'j', [0x25] = 'k', [0x26] = 'l', [0x27] = ';',
	[0x28] = '\'', [0x29] = '`', [0x2B] = '\\',
	[0x2C] = 'z', [0x2D] = 'x', [0x2E] = 'c', [0x2F] = 'v', [0x30] = 'b',
	[0x31] = 'n', [0x32] = 'm', [0x33] = ',', [0x34] = '.', [0x35] = '/',
	[0x39] = ' ',
};

static const char map_shifted[128] = {
	[0x02] = '!', [0x03] = '@', [0x04] = '#', [0x05] = '$', [0x06] = '%',
	[0x07] = '^', [0x08] = '&', [0x09] = '*', [0x0A] = '(', [0x0B] = ')',
	[0x0C] = '_', [0x0D] = '+', [0x0E] = '\b', [0x0F] = '\t',
	[0x10] = 'Q', [0x11] = 'W', [0x12] = 'E', [0x13] = 'R', [0x14] = 'T',
	[0x15] = 'Y', [0x16] = 'U', [0x17] = 'I', [0x18] = 'O', [0x19] = 'P',
	[0x1A] = '{', [0x1B] = '}', [0x1C] = '\n',
	[0x1E] = 'A', [0x1F] = 'S', [0x20] = 'D', [0x21] = 'F', [0x22] = 'G',
	[0x23] = 'H', [0x24] = 'J', [0x25] = 'K', [0x26] = 'L', [0x27] = ':',
	[0x28] = '"', [0x29] = '~', [0x2B] = '|',
	[0x2C] = 'Z', [0x2D] = 'X', [0x2E] = 'C', [0x2F] = 'V', [0x30] = 'B',
	[0x31] = 'N', [0x32] = 'M', [0x33] = '<', [0x34] = '>', [0x35] = '?',
	[0x39] = ' ',
};

/** Push one key (ASCII or KBD_UP / KBD_DOWN). Drops input if full. */
static void kbd_push(int c)
{
	unsigned next = (qhead + 1) % KBD_BUF_SIZE;
	if (next == qtail) {
		return;
	}
	queue[qhead] = c;
	qhead = next;
}

/** True when the 8042 looks absent (floating bus). */
static int kbd_absent(void)
{
	uint8_t st = inb(KBD_STATUS);
	return st == 0xFF;
}

/** Wait until the 8042 input buffer can take a command/data byte. */
static int kbd_in_ready(uint32_t ms)
{
	uint64_t deadline = pit_ticks() + ms;
	while (pit_ticks() < deadline) {
		if ((inb(KBD_STATUS) & ST_IN) == 0) {
			return 1;
		}
		io_wait();
	}
	return 0;
}

/** Wait until the 8042 output buffer has a byte. */
static int kbd_out_ready(uint32_t ms)
{
	uint64_t deadline = pit_ticks() + ms;
	while (pit_ticks() < deadline) {
		if (inb(KBD_STATUS) & ST_OUT) {
			return 1;
		}
		io_wait();
	}
	return 0;
}

/** Discard any bytes sitting in the output buffer (not as key events). */
static void kbd_flush(void)
{
	for (int i = 0; i < 64; i++) {
		if ((inb(KBD_STATUS) & ST_OUT) == 0) {
			return;
		}
		(void)inb(KBD_DATA);
		io_wait();
	}
}

/** Write a controller command to port 0x64. */
static int kbd_cmd(uint8_t cmd)
{
	if (!kbd_in_ready(50)) {
		return 0;
	}
	outb(KBD_STATUS, cmd);
	return 1;
}

/** Write a data byte to port 0x60. */
static int kbd_data(uint8_t v)
{
	if (!kbd_in_ready(50)) {
		return 0;
	}
	outb(KBD_DATA, v);
	return 1;
}

/** Read one controller/device byte, or -1 on timeout. */
static int kbd_read(uint32_t ms)
{
	if (!kbd_out_ready(ms)) {
		return -1;
	}
	return (int)inb(KBD_DATA);
}

/**
 * Reprogram the 8042 and the keyboard after BIOS USB boot / EHCI handoff.
 * Interrupts are held off so reset ACKs are not treated as scancodes.
 */
void kbd_init(void)
{
	unsigned long rflags;
	__asm__ volatile ("pushfq; pop %0; cli" : "=r"(rflags));

	qhead = 0;
	qtail = 0;
	shift = 0;
	ctrl = 0;
	extended = 0;
	held_left = 0;
	held_right = 0;
	held_down = 0;
	held_up = 0;
	held_pgup = 0;
	held_pgdn = 0;

	if (kbd_absent()) {
		goto out;
	}

	kbd_cmd(CC_DISABLE_KBD);
	kbd_cmd(CC_DISABLE_AUX);
	kbd_flush();

	if (!kbd_cmd(CC_READ_CFG)) {
		goto out;
	}
	int cfg = kbd_read(50);
	if (cfg < 0) {
		cfg = CFG_INT1 | CFG_SYS | CFG_XLAT;
	}
	cfg |= CFG_INT1 | CFG_XLAT | CFG_SYS;
	cfg &= (int)~CFG_DISABLE_KBD;
	if (!kbd_cmd(CC_WRITE_CFG) || !kbd_data((uint8_t)cfg)) {
		goto out;
	}

	kbd_cmd(CC_ENABLE_KBD);
	kbd_flush();

	/* Device reset: ACK then BAT. Missing replies still leave the port on. */
	if (kbd_data(KD_RESET)) {
		(void)kbd_read(200);
		(void)kbd_read(500);
	}
	kbd_flush();
	if (kbd_data(KD_ENABLE)) {
		(void)kbd_read(100);
	}
	kbd_flush();

out:
	if (rflags & 0x200) {
		__asm__ volatile ("sti");
	}
}

/** Translate a scancode into ASCII and queue it. */
static void kbd_handle(uint8_t sc)
{
	if (sc == 0xE0) {
		extended = 1;
		return;
	}
	int release = (sc & 0x80) != 0;
	uint8_t code = sc & 0x7F;
	if (extended) {
		extended = 0;
		/* Keep DAS / soft-drop accurate: NES needs key-up, not only taps. */
		if (code == 0x4B) {
			held_left = release ? 0 : 1;
		} else if (code == 0x4D) {
			held_right = release ? 0 : 1;
		} else if (code == 0x50) {
			held_down = release ? 0 : 1;
		} else if (code == 0x48) {
			held_up = release ? 0 : 1;
		} else if (code == 0x49) {
			held_pgup = release ? 0 : 1;
		} else if (code == 0x51) {
			held_pgdn = release ? 0 : 1;
		}
		if (release) {
			return;
		}
		if (code == 0x48) {
			kbd_push(KBD_UP);
		} else if (code == 0x50) {
			kbd_push(KBD_DOWN);
		} else if (code == 0x4B) {
			kbd_push(KBD_LEFT);
		} else if (code == 0x4D) {
			kbd_push(KBD_RIGHT);
		} else if (code == 0x49) {
			kbd_push(KBD_PGUP);
		} else if (code == 0x51) {
			kbd_push(KBD_PGDN);
		} else if (code == 0x47) {
			kbd_push(KBD_HOME);
		} else if (code == 0x4F) {
			kbd_push(KBD_END);
		}
		return;
	}
	if (code == 0x1D) {
		ctrl = release ? 0 : 1;
		return;
	}
	if (code == 0x2A || code == 0x36) {
		shift = release ? 0 : 1;
		return;
	}
	if (release) {
		return;
	}
	char ch = shift ? map_shifted[code] : map_unshifted[code];
	if (ch == 0) {
		return;
	}
	if (ctrl && ch >= 'a' && ch <= 'z') {
		kbd_push((int)(ch - 'a' + 1));
		return;
	}
	if (ctrl && ch >= 'A' && ch <= 'Z') {
		kbd_push((int)(ch - 'A' + 1));
		return;
	}
	kbd_push((unsigned char)ch);
}

/** IRQ1 path: consume the data port. Harmless if the poll loop already did. */
void kbd_irq(void)
{
	if (inb(KBD_STATUS) & ST_OUT) {
		kbd_handle(inb(KBD_DATA));
	}
}

/** Drain any pending controller bytes into the ASCII queue. */
void kbd_poll(void)
{
	if (kbd_absent()) {
		return;
	}
	for (int n = 0; n < 32; n++) {
		if ((inb(KBD_STATUS) & ST_OUT) == 0) {
			return;
		}
		kbd_handle(inb(KBD_DATA));
	}
}

/** Pop one queued ASCII byte, or -1 if empty. Polls the controller first. */
int kbd_getc(void)
{
	kbd_poll();
	if (qhead == qtail) {
		return -1;
	}
	int key = queue[qtail];
	qtail = (qtail + 1) % KBD_BUF_SIZE;
	return key;
}

/** Arrow hold state for games that implement NES DAS. */
int kbd_held(int key)
{
	kbd_poll();
	if (key == KBD_LEFT) {
		return held_left;
	}
	if (key == KBD_RIGHT) {
		return held_right;
	}
	if (key == KBD_DOWN) {
		return held_down;
	}
	if (key == KBD_UP) {
		return held_up;
	}
	if (key == KBD_PGUP) {
		return held_pgup;
	}
	if (key == KBD_PGDN) {
		return held_pgdn;
	}
	return 0;
}

void kbd_flush_queue(void)
{
	kbd_poll();
	qhead = 0;
	qtail = 0;
}

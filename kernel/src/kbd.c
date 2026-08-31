#include "kbd.h"
#include "io.h"

#include <stdint.h>

#define KBD_DATA	0x60
#define KBD_STATUS	0x64
#define KBD_BUF_SIZE	128

static char queue[KBD_BUF_SIZE];
static unsigned qhead;
static unsigned qtail;
static int shift;
static int extended;

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

/** Push one ASCII byte into the keyboard ring. Drops input if full. */
static void kbd_push(char c)
{
	unsigned next = (qhead + 1) % KBD_BUF_SIZE;
	if (next == qtail) {
		return;
	}
	queue[qhead] = c;
	qhead = next;
}

/** Drain leftover scancodes so IRQ1 starts from a clean controller. */
void kbd_init(void)
{
	qhead = 0;
	qtail = 0;
	shift = 0;
	extended = 0;
	while (inb(KBD_STATUS) & 0x01) {
		(void)inb(KBD_DATA);
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
	if (code == 0x2A || code == 0x36) {
		shift = release ? 0 : 1;
		extended = 0;
		return;
	}
	if (release || extended) {
		extended = 0;
		return;
	}
	char ch = shift ? map_shifted[code] : map_unshifted[code];
	if (ch != 0) {
		kbd_push(ch);
	}
}

/** IRQ1 path: consume the data port. Harmless if the poll loop already did. */
void kbd_irq(void)
{
	if (inb(KBD_STATUS) & 0x01) {
		kbd_handle(inb(KBD_DATA));
	}
}

/** Drain any pending controller bytes into the ASCII queue. */
void kbd_poll(void)
{
	while (inb(KBD_STATUS) & 0x01) {
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
	char c = queue[qtail];
	qtail = (qtail + 1) % KBD_BUF_SIZE;
	return (unsigned char)c;
}

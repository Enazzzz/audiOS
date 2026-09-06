#include "kbd.h"
#include "io.h"

#define KBD_DATA	0x60
#define KBD_STATUS	0x64
#define KBD_BUF		64

static int queue[KBD_BUF];
static unsigned qh, qt;
static int shift;
static int ctrl;
static int ext;

static const char unshifted[128] = {
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

static const char shifted[128] = {
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

/** Push one decoded key. */
static void kbd_push(int c)
{
	unsigned n = (qh + 1u) % KBD_BUF;
	if (n != qt) {
		queue[qh] = c;
		qh = n;
	}
}

void kbd_inject(int key)
{
	kbd_push(key);
}

void kbd_init(void)
{
	qh = qt = 0;
	/* Enable keyboard clock + IRQ1 + translate. */
	while (inb(KBD_STATUS) & 2) {
	}
	outb(KBD_STATUS, 0xAE);
	while (inb(KBD_STATUS) & 2) {
	}
	outb(KBD_STATUS, 0x60);
	while (inb(KBD_STATUS) & 2) {
	}
	outb(KBD_DATA, 0x45);
}

void kbd_irq(void)
{
	uint8_t sc;
	if ((inb(KBD_STATUS) & 1) == 0) {
		return;
	}
	sc = inb(KBD_DATA);
	if (sc == 0xE0) {
		ext = 1;
		return;
	}
	if (sc == 0x2A || sc == 0x36) {
		shift = 1;
		return;
	}
	if (sc == 0xAA || sc == 0xB6) {
		shift = 0;
		return;
	}
	if (sc == 0x1D) {
		ctrl = 1;
		return;
	}
	if (sc == 0x9D) {
		ctrl = 0;
		return;
	}
	if (sc & 0x80) {
		ext = 0;
		return;
	}
	if (ext) {
		ext = 0;
		if (sc == 0x48) {
			kbd_push(KBD_UP);
		} else if (sc == 0x50) {
			kbd_push(KBD_DOWN);
		}
		return;
	}
	if (ctrl && sc == 0x2D) {
		kbd_push(0x18);	/* Ctrl-X */
		return;
	}
	if (sc < 128) {
		char ch = shift ? shifted[sc] : unshifted[sc];
		if (ch) {
			if (ctrl && ch >= 'a' && ch <= 'z') {
				kbd_push(ch - 'a' + 1);
			} else {
				kbd_push(ch);
			}
		}
	}
}

int kbd_getc(void)
{
	int c;
	if (qh == qt) {
		return -1;
	}
	c = queue[qt];
	qt = (qt + 1u) % KBD_BUF;
	return c;
}

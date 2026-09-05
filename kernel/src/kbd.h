#ifndef AUDIOS_KBD_H
#define AUDIOS_KBD_H

/** Special keys (not ASCII) returned by `kbd_getc`. */
#define KBD_UP		0x110
#define KBD_DOWN	0x111
#define KBD_LEFT	0x112
#define KBD_RIGHT	0x113
#define KBD_PGUP	0x114
#define KBD_PGDN	0x115
#define KBD_HOME	0x116
#define KBD_END		0x117

/** Enable the 8042 keyboard port (clock, IRQ1, set-1 translation) and reset the device. */
void kbd_init(void);

/** IRQ1 handler: translate scancode set 1 into ASCII and queue it. */
void kbd_irq(void);

/** Non-blocking key read. Returns -1 if empty, ASCII, Ctrl+letter as 1–26, or KBD_* . */
int kbd_getc(void);

/**
 * True while a PS/2 arrow is physically down (scancode release tracked).
 * Serial terminals never send key-up, so this stays 0 for COM1.
 */
int kbd_held(int key);

/** Drop any queued key so a full-screen app does not eat a leftover character. */
void kbd_flush_queue(void);

#endif

#ifndef AUDIOS_KBD_H
#define AUDIOS_KBD_H

/** Special keys (not ASCII) returned by `kbd_getc`. */
#define KBD_UP		0x110
#define KBD_DOWN	0x111

/** Enable the 8042 keyboard port (clock, IRQ1, set-1 translation) and reset the device. */
void kbd_init(void);

/** IRQ1 handler: translate scancode set 1 into ASCII and queue it. */
void kbd_irq(void);

/** Non-blocking key read. Returns -1 if empty, ASCII, or KBD_UP / KBD_DOWN. */
int kbd_getc(void);

/** Queue a key from USB HID (same values as `kbd_getc`). */
void kbd_inject(int c);

#endif

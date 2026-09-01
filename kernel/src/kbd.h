#ifndef AUDIOS_KBD_H
#define AUDIOS_KBD_H

/** Special keys (not ASCII) returned by `kbd_getc`. */
#define KBD_UP		0x110
#define KBD_DOWN	0x111

/** Drain the PS/2 data port and enable IRQ1 handling. */
void kbd_init(void);

/** IRQ1 handler: translate scancode set 1 into ASCII and queue it. */
void kbd_irq(void);

/** Non-blocking key read. Returns -1 if empty, ASCII, or KBD_UP / KBD_DOWN. */
int kbd_getc(void);

#endif

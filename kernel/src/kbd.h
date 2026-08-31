#ifndef AUDIOS_KBD_H
#define AUDIOS_KBD_H

/** Drain the PS/2 data port and enable IRQ1 handling. */
void kbd_init(void);

/** IRQ1 handler: translate scancode set 1 into ASCII and queue it. */
void kbd_irq(void);

/** Non-blocking ASCII read. Returns -1 if the input queue is empty. */
int kbd_getc(void);

#endif

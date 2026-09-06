#ifndef AUDIOS_KBD_H
#define AUDIOS_KBD_H

#define KBD_UP		0x110
#define KBD_DOWN	0x111
#define KBD_LEFT	0x112
#define KBD_RIGHT	0x113
#define KBD_F5		0x118

/** Drain the 8042 and enable scan-code translation + IRQ1. */
void kbd_init(void);
/** Consume one scancode (called from IRQ1). */
void kbd_irq(void);
/** Pop one decoded key, or -1 if empty. */
int kbd_getc(void);
/** Inject a key as if it came from the keyboard (Audio Link). */
void kbd_inject(int key);

#endif

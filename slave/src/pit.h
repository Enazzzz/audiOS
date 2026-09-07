#ifndef AUDIOS_PIT_H
#define AUDIOS_PIT_H

#include <stdint.h>

#define PIT_HZ	1000u

void pit_init(void);
void pit_irq(void);
/** Milliseconds since `pit_init` (IRQ0). Wraps at 2^32 ms. */
uint32_t pit_ticks(void);

#endif

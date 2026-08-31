#ifndef AUDIOS_PIT_H
#define AUDIOS_PIT_H

#include <stdint.h>

#define PIT_HZ	1000u

/** Program the PIT, calibrate the TSC, and start counting time. */
void pit_init(void);

/** IRQ0 handler: increment the fallback tick counter. */
void pit_irq(void);

/** Milliseconds since `pit_init`, from the calibrated TSC. */
uint64_t pit_ticks(void);

#endif

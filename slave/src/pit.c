#include "pit.h"
#include "io.h"

static volatile uint32_t irq_ticks;

/** 1000 Hz PIT channel 0. Time is IRQ ticks (no 64-bit TSC divide on i386). */
void pit_init(void)
{
	const uint32_t divisor = 1193182u / PIT_HZ;
	outb(0x43, 0x36);
	outb(0x40, (uint8_t)(divisor & 0xFF));
	outb(0x40, (uint8_t)((divisor >> 8) & 0xFF));
	irq_ticks = 0;
}

void pit_irq(void)
{
	irq_ticks++;
}

uint32_t pit_ticks(void)
{
	return irq_ticks;
}

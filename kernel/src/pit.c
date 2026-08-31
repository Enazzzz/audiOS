#include "pit.h"
#include "io.h"

static volatile uint64_t irq_ticks;
static uint64_t tsc_hz;
static uint64_t tsc_start;

/** Read the timestamp counter. Deterministic enough for CLI uptime. */
static uint64_t rdtsc(void)
{
	uint32_t lo;
	uint32_t hi;
	__asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
	return ((uint64_t)hi << 32) | lo;
}

/**
 * Measure TSC frequency against a 10 ms PIT channel-2 one-shot.
 * Channel 2 is polled on port 0x61, so this does not depend on IRQs.
 */
static uint64_t calibrate_tsc(void)
{
	const uint16_t count = 11932;	/* ~10 ms at 1.193182 MHz */
	uint8_t speaker = inb(0x61);
	outb(0x61, (uint8_t)((speaker & ~0x02) | 0x01));
	outb(0x43, 0xB0);
	outb(0x42, (uint8_t)(count & 0xFF));
	outb(0x42, (uint8_t)((count >> 8) & 0xFF));
	uint64_t t0 = rdtsc();
	while ((inb(0x61) & 0x20) == 0) {
		__asm__ volatile ("pause");
	}
	uint64_t t1 = rdtsc();
	outb(0x61, speaker);
	uint64_t delta = t1 - t0;
	if (delta < 1000) {
		return 0;
	}
	return (delta * 1193182ull) / count;
}

/** Configure channel 0 as a square wave and start TSC-based timekeeping. */
void pit_init(void)
{
	const uint32_t divisor = 1193182u / PIT_HZ;
	outb(0x43, 0x36);
	outb(0x40, (uint8_t)(divisor & 0xFF));
	outb(0x40, (uint8_t)((divisor >> 8) & 0xFF));
	irq_ticks = 0;
	tsc_hz = calibrate_tsc();
	tsc_start = rdtsc();
}

/** Advance the IRQ fallback counter when IRQ0 actually arrives. */
void pit_irq(void)
{
	irq_ticks++;
}

/** Return milliseconds since init. Prefers TSC; falls back to IRQ ticks. */
uint64_t pit_ticks(void)
{
	if (tsc_hz != 0) {
		uint64_t elapsed = rdtsc() - tsc_start;
		return elapsed * 1000ull / tsc_hz;
	}
	return irq_ticks;
}

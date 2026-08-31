#include "reboot.h"
#include "io.h"
#include "tty.h"

/**
 * Ask the 8042 keyboard controller to pulse the reset line. If that does
 * not take the machine down, load an empty IDT and fire an interrupt so
 * the CPU triple-faults into a reset.
 */
void system_reboot(void)
{
	tty_puts("rebooting...\n");
	__asm__ volatile ("cli");
	for (int i = 0; i < 100000; i++) {
		if ((inb(0x64) & 0x02) == 0) {
			break;
		}
		io_wait();
	}
	outb(0x64, 0xFE);
	io_wait();
	struct {
		uint16_t limit;
		uint64_t base;
	} __attribute__((packed)) empty = { 0, 0 };
	__asm__ volatile ("lidt %0; int $0" : : "m"(empty));
	for (;;) {
		__asm__ volatile ("hlt");
	}
}

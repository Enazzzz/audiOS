#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <limine.h>

#include "audio.h"
#include "cpu.h"
#include "idt.h"
#include "kbd.h"
#include "meminfo.h"
#include "pic.h"
#include "pit.h"
#include "serial.h"
#include "shell.h"
#include "tty.h"

__attribute__((used, section(".limine_requests")))
static volatile uint64_t limine_base_revision[] = LIMINE_BASE_REVISION(6);

__attribute__((used, section(".limine_requests")))
static volatile struct limine_framebuffer_request framebuffer_request = {
	.id = LIMINE_FRAMEBUFFER_REQUEST_ID,
	.revision = 0
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_memmap_request memmap_request = {
	.id = LIMINE_MEMMAP_REQUEST_ID,
	.revision = 0
};

__attribute__((used, section(".limine_requests_start")))
static volatile uint64_t limine_requests_start_marker[] = LIMINE_REQUESTS_START_MARKER;

__attribute__((used, section(".limine_requests_end")))
static volatile uint64_t limine_requests_end_marker[] = LIMINE_REQUESTS_END_MARKER;

/** Halt forever. Used when the bootloader handshake fails. */
static void hcf(void)
{
	for (;;) {
		__asm__ volatile ("cli; hlt");
	}
}

/**
 * Kernel entry. Limine has already placed us in 64-bit mode with a stack.
 * Bring up console, interrupts, timer, keyboard, CPU/memory probes, and the
 * audio system object, then hand control to the shell.
 */
void kmain(void)
{
	if (LIMINE_BASE_REVISION_SUPPORTED(limine_base_revision) == false) {
		hcf();
	}

	serial_init();

	if (framebuffer_request.response == NULL
		|| framebuffer_request.response->framebuffer_count < 1) {
		serial_putc('!');
		hcf();
	}

	tty_init(framebuffer_request.response->framebuffers[0]);
	meminfo_init(memmap_request.response);

	struct cpu_info cpu;
	cpu_detect(&cpu);
	(void)cpu;

	pic_init();
	idt_init();
	pit_init();
	kbd_init();
	audio_init();

	__asm__ volatile ("sti");
	shell_run();
	hcf();
}

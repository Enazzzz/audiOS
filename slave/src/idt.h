#ifndef AUDIOS_IDT_H
#define AUDIOS_IDT_H

#include <stdint.h>

/**
 * 32-bit interrupt frame matching `entry.asm`:
 * `pusha`, then `push ds`, `push es` (es at the lowest address), then
 * the CPU-pushed vector/error/eip/cs/eflags.
 */
struct interrupt_frame {
	uint32_t es, ds;
	uint32_t edi, esi, ebp, esp, ebx, edx, ecx, eax;
	uint32_t vector, error;
	uint32_t eip, cs, eflags;
};

/** Install 48 protected-mode interrupt gates (exceptions + IRQ0–15). */
void idt_init(void);

void interrupt_dispatch(struct interrupt_frame *frame);

#endif

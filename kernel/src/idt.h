#ifndef AUDIOS_IDT_H
#define AUDIOS_IDT_H

#include <stdint.h>

/** Saved registers and CPU-pushed frame presented to the C dispatcher. */
struct interrupt_frame {
	uint64_t rax, rbx, rcx, rdx, rsi, rdi, rbp;
	uint64_t r8, r9, r10, r11, r12, r13, r14, r15;
	uint64_t vector, error;
	uint64_t rip, cs, rflags, rsp, ss;
};

/** Install the IDT and exception / IRQ stubs. */
void idt_init(void);

/** C dispatcher invoked from the assembly common stub. */
void interrupt_dispatch(struct interrupt_frame *frame);

#endif

#include "idt.h"
#include "kbd.h"
#include "pic.h"
#include "pit.h"
#include "tty.h"

#include <stdint.h>

extern void *isr_stub_table[256];

struct idt_entry {
	uint16_t offset_lo;
	uint16_t selector;
	uint8_t ist;
	uint8_t flags;
	uint16_t offset_mid;
	uint32_t offset_hi;
	uint32_t reserved;
} __attribute__((packed));

struct idt_ptr {
	uint16_t limit;
	uint64_t base;
} __attribute__((packed));

static struct idt_entry idt[256];

/** Program one interrupt gate using the current kernel code selector. */
static void idt_set_gate(uint8_t vec, uint64_t handler, uint16_t cs)
{
	idt[vec].offset_lo = (uint16_t)(handler & 0xFFFF);
	idt[vec].selector = cs;
	idt[vec].ist = 0;
	idt[vec].flags = 0x8E;	/* present, ring 0, 64-bit interrupt gate */
	idt[vec].offset_mid = (uint16_t)((handler >> 16) & 0xFFFF);
	idt[vec].offset_hi = (uint32_t)(handler >> 32);
	idt[vec].reserved = 0;
}

/** Load the IDT with stubs for every vector. */
void idt_init(void)
{
	uint16_t cs;
	__asm__ volatile ("mov %%cs, %0" : "=r"(cs));
	for (int i = 0; i < 256; i++) {
		idt_set_gate((uint8_t)i, (uint64_t)isr_stub_table[i], cs);
	}
	struct idt_ptr ptr = {
		.limit = (uint16_t)(sizeof(idt) - 1),
		.base = (uint64_t)&idt[0],
	};
	__asm__ volatile ("lidt %0" : : "m"(ptr));
}

/** Dispatch exceptions and IRQs. Called from the assembly common stub. */
void interrupt_dispatch(struct interrupt_frame *frame)
{
	uint8_t vector = (uint8_t)frame->vector;
	if (vector < 32) {
		tty_set_color(TTY_COL_ERR);
		tty_printf("\nexception %u  error 0x%lx  rip 0x%lx\n",
			(unsigned)vector, (unsigned long)frame->error,
			(unsigned long)frame->rip);
		for (;;) {
			__asm__ volatile ("cli; hlt");
		}
	}
	if (vector == PIC_IRQ_BASE) {
		pit_irq();
	} else if (vector == PIC_IRQ_BASE + 1) {
		kbd_irq();
	}
	pic_eoi(vector);
}

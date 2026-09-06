#include "idt.h"
#include "kbd.h"
#include "pic.h"
#include "pit.h"
#include "tty.h"

#include <stdint.h>

extern void *isr_stub_table[48];

struct idt_entry {
	uint16_t offset_lo;
	uint16_t selector;
	uint8_t zero;
	uint8_t flags;
	uint16_t offset_hi;
} __attribute__((packed));

struct idt_ptr {
	uint16_t limit;
	uint32_t base;
} __attribute__((packed));

static struct idt_entry idt[256];

/** Program one 32-bit interrupt gate. */
static void idt_set_gate(uint8_t vec, uint32_t handler)
{
	idt[vec].offset_lo = (uint16_t)(handler & 0xFFFF);
	idt[vec].selector = 0x08;
	idt[vec].zero = 0;
	idt[vec].flags = 0x8E;
	idt[vec].offset_hi = (uint16_t)(handler >> 16);
}

void idt_init(void)
{
	unsigned i;
	for (i = 0; i < 48; i++) {
		idt_set_gate((uint8_t)i, (uint32_t)isr_stub_table[i]);
	}
	struct idt_ptr ptr = {
		.limit = (uint16_t)(sizeof(idt) - 1),
		.base = (uint32_t)&idt[0],
	};
	__asm__ volatile ("lidt %0" : : "m"(ptr));
}

/** IRQ0 ticks the PIT; IRQ1 is the PS/2 keyboard. Faults halt with a VGA line. */
void interrupt_dispatch(struct interrupt_frame *frame)
{
	uint8_t vector = (uint8_t)frame->vector;
	if (vector < 32) {
		tty_set_color(TTY_COL_ERR);
		tty_printf("\nexception %u eip 0x%x\n",
			(unsigned)vector, (unsigned)frame->eip);
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

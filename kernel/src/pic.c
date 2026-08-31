#include "pic.h"
#include "io.h"

#define PIC1_CMD	0x20
#define PIC1_DATA	0x21
#define PIC2_CMD	0xA0
#define PIC2_DATA	0xA1
#define PIC_EOI		0x20

/**
 * Remap IRQs off the exception range and leave only the timer and keyboard
 * unmasked. IRQ2 (cascade) stays masked because slave IRQs are unused in 0.1.
 */
void pic_init(void)
{
	outb(PIC1_CMD, 0x11);
	io_wait();
	outb(PIC2_CMD, 0x11);
	io_wait();
	outb(PIC1_DATA, PIC_IRQ_BASE);
	io_wait();
	outb(PIC2_DATA, PIC_IRQ_BASE + 8);
	io_wait();
	outb(PIC1_DATA, 0x04);
	io_wait();
	outb(PIC2_DATA, 0x02);
	io_wait();
	outb(PIC1_DATA, 0x01);
	io_wait();
	outb(PIC2_DATA, 0x01);
	io_wait();
	outb(PIC1_DATA, 0xFC);	/* unmask IRQ0 + IRQ1 */
	outb(PIC2_DATA, 0xFF);
}

/** Acknowledge the PIC that raised `vector`. */
void pic_eoi(uint8_t vector)
{
	if (vector >= PIC_IRQ_BASE + 8) {
		outb(PIC2_CMD, PIC_EOI);
	}
	if (vector >= PIC_IRQ_BASE) {
		outb(PIC1_CMD, PIC_EOI);
	}
}

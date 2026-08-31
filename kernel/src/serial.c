#include "serial.h"
#include "io.h"

#define COM1		0x3F8
#define COM1_IER	(COM1 + 1)
#define COM1_FCR	(COM1 + 2)
#define COM1_LCR	(COM1 + 3)
#define COM1_MCR	(COM1 + 4)
#define COM1_LSR	(COM1 + 5)

/** Program COM1 for 115200 baud, 8 data bits, no parity, one stop bit. */
void serial_init(void)
{
	outb(COM1_IER, 0x00);
	outb(COM1_LCR, 0x80);
	outb(COM1, 0x01);		/* 115200 divisor 1 */
	outb(COM1_IER, 0x00);
	outb(COM1_LCR, 0x03);
	outb(COM1_FCR, 0xC7);
	outb(COM1_MCR, 0x0B);
}

/** Block until the UART can accept a byte, then write it. */
void serial_putc(char c)
{
	if (c == '\n') {
		serial_putc('\r');
	}
	while ((inb(COM1_LSR) & 0x20) == 0) {
		/* spin */
	}
	outb(COM1, (uint8_t)c);
}

/** Return the next received byte, or -1 if the UART FIFO is empty. */
int serial_getc(void)
{
	if ((inb(COM1_LSR) & 0x01) == 0) {
		return -1;
	}
	return (int)inb(COM1);
}

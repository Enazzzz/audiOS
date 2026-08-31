#ifndef AUDIOS_IO_H
#define AUDIOS_IO_H

#include <stdint.h>

/** Read an 8-bit value from an I/O port. */
static inline uint8_t inb(uint16_t port)
{
	uint8_t value;
	__asm__ volatile ("inb %1, %0" : "=a"(value) : "Nd"(port));
	return value;
}

/** Write an 8-bit value to an I/O port. */
static inline void outb(uint16_t port, uint8_t value)
{
	__asm__ volatile ("outb %0, %1" : : "a"(value), "Nd"(port));
}

/** Read a 16-bit value from an I/O port. */
static inline uint16_t inw(uint16_t port)
{
	uint16_t value;
	__asm__ volatile ("inw %1, %0" : "=a"(value) : "Nd"(port));
	return value;
}

/** Write a 16-bit value to an I/O port. */
static inline void outw(uint16_t port, uint16_t value)
{
	__asm__ volatile ("outw %0, %1" : : "a"(value), "Nd"(port));
}

/** Read a 32-bit value from an I/O port. */
static inline uint32_t inl(uint16_t port)
{
	uint32_t value;
	__asm__ volatile ("inl %1, %0" : "=a"(value) : "Nd"(port));
	return value;
}

/** Write a 32-bit value to an I/O port. */
static inline void outl(uint16_t port, uint32_t value)
{
	__asm__ volatile ("outl %0, %1" : : "a"(value), "Nd"(port));
}

/** Brief delay used after PIC / keyboard-controller writes. */
static inline void io_wait(void)
{
	outb(0x80, 0);
}

#endif

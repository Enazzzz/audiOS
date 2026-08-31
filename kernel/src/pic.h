#ifndef AUDIOS_PIC_H
#define AUDIOS_PIC_H

#include <stdint.h>

#define PIC_IRQ_BASE	32

/** Remap the legacy PICs so IRQ0..15 land on IDT vectors 32..47. */
void pic_init(void);

/** Send end-of-interrupt for a CPU vector in the IRQ range. */
void pic_eoi(uint8_t vector);

#endif

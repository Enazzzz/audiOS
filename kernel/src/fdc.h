#ifndef AUDIOS_FDC_H
#define AUDIOS_FDC_H

#include <stdint.h>

/** 1.44 MiB PC floppy: 80 cyl × 2 heads × 18 sectors × 512. */
#define FDC_SECSZ	512u
#define FDC_SPT		18u
#define FDC_HEADS	2u
#define FDC_CYLS	80u
#define FDC_SECTORS	(FDC_CYLS * FDC_HEADS * FDC_SPT)

/** Probe the 82077-compatible controller at 0x3F0. Safe if the chip is absent. */
void fdc_init(void (*idle)(void));

/** IRQ6 completion (PIC vector 38). */
void fdc_irq(void);

int fdc_present(void);

/** Low-level format of every track (destroys the disk). */
int fdc_format_disk(void (*idle)(void));

/** Read or write one 512-byte sector. `lba` is 0..2879. */
int fdc_read(uint32_t lba, void *buf);
int fdc_write(uint32_t lba, const void *buf);

/** Stop the selected unit. Call when a multi-sector copy finishes. */
void fdc_motor_off(void);

/** Selected FDC unit (0 or 1). */
unsigned fdc_unit(void);

/** Sense Drive Status (ST3). Spins the motor briefly if it is off. */
int fdc_sense_drive(uint8_t *st3);

const char *fdc_error(void);

#endif

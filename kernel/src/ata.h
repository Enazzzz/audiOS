#ifndef AUDIOS_ATA_H
#define AUDIOS_ATA_H

#include <stdint.h>

#define ATA_MAX_DEV	4u

struct ata_dev {
	/** Command block base (0x1F0 / 0x170 or a PCI BAR). */
	uint16_t io;
	/** Device-control / alt-status (0x3F6 / 0x376). */
	uint16_t ctrl;
	/** 0 = master, 1 = slave. */
	uint8_t slave;
	char model[44];
	uint32_t sectors;
};

/** Probe legacy 0x1F0/0x170 and PCI IDE BARs. Safe with no disk. */
void ata_init(void);

unsigned ata_count(void);
const struct ata_dev *ata_at(unsigned index);

/** PIO read `count` sectors at `lba` into `buf` (512 * count bytes). */
int ata_read(unsigned index, uint32_t lba, uint32_t count, void *buf);

/** PIO write `count` sectors. */
int ata_write(unsigned index, uint32_t lba, uint32_t count, const void *buf);

/** FLUSH CACHE (0xE7). Best-effort; returns 0 only on a hard timeout. */
int ata_flush(unsigned index);

const char *ata_error(void);

#endif

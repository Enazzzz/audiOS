#ifndef AUDIOS_BLK_H
#define AUDIOS_BLK_H

#include <stdint.h>

/** Synchronous block device. Sector size is 512. */
struct blkdev {
	int (*read)(uint64_t lba, uint32_t count, void *buf);
	int (*write)(uint64_t lba, uint32_t count, const void *buf);
	uint64_t sectors;
	char name[40];
};

#endif

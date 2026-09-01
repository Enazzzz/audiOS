#ifndef AUDIOS_FAT_H
#define AUDIOS_FAT_H

#include "blk.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define FAT_NAME_MAX	80
#define FAT_PATH_MAX	128

enum fat_kind {
	FAT_NONE = 0,
	FAT_FILE,
	FAT_DIR
};

struct fat_info {
	enum fat_kind kind;
	uint32_t size;
	uint32_t cluster;
	char name[FAT_NAME_MAX];
};

struct fat_iter {
	uint32_t dir_clus;
	uint32_t index;
};

/** Mount a FAT32 partition on `dev`. Does not create or repair a volume. */
bool fat_mount(struct blkdev *dev);

bool fat_mounted(void);
const char *fat_volume(void);
uint64_t fat_bytes_total(void);
uint64_t fat_bytes_free(void);
const char *fat_last_error(void);

/** Resolve `path` (absolute, `/`-separated) into `out`. */
bool fat_stat(const char *path, struct fat_info *out);

/** Start listing an absolute directory path. */
bool fat_iter_begin(const char *dir_path, struct fat_iter *it);

/** Next directory entry. Returns false at end or on error. */
bool fat_iter_next(struct fat_iter *it, struct fat_info *out);

bool fat_read(const char *path, void *buf, uint32_t cap, uint32_t *out_size);
bool fat_write(const char *path, const void *buf, uint32_t size);
bool fat_touch(const char *path);
bool fat_mkdir(const char *path);
bool fat_remove(const char *path);
bool fat_rename(const char *src, const char *dst);
bool fat_copy(const char *src, const char *dst);

#endif

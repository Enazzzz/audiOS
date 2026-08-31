#ifndef AUDIOS_MEMINFO_H
#define AUDIOS_MEMINFO_H

#include <limine.h>
#include <stdint.h>

struct mem_info {
	uint64_t usable_bytes;
	uint64_t total_bytes;
	uint64_t entry_count;
};

/** Summarise the Limine physical memory map. */
void meminfo_init(struct limine_memmap_response *map);

/** Print the memory summary used by the `mem` command. */
void meminfo_print(void);

/** Return the last summarised map, or NULL before `meminfo_init`. */
const struct mem_info *meminfo_get(void);

#endif

#include "meminfo.h"
#include "tty.h"

#include <stddef.h>

static struct mem_info info;
static int ready;

/** Walk the bootloader map and cache usable / total byte counts. */
void meminfo_init(struct limine_memmap_response *map)
{
	info.usable_bytes = 0;
	info.total_bytes = 0;
	info.entry_count = 0;
	ready = 0;
	if (map == NULL) {
		return;
	}
	info.entry_count = map->entry_count;
	for (uint64_t i = 0; i < map->entry_count; i++) {
		struct limine_memmap_entry *e = map->entries[i];
		if (e->type == LIMINE_MEMMAP_USABLE) {
			info.usable_bytes += e->length;
			info.total_bytes += e->length;
		} else if (e->type == LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE
			|| e->type == LIMINE_MEMMAP_EXECUTABLE_AND_MODULES
			|| e->type == LIMINE_MEMMAP_ACPI_RECLAIMABLE
			|| e->type == LIMINE_MEMMAP_ACPI_NVS) {
			info.total_bytes += e->length;
		}
	}
	ready = 1;
}

/** Return the cached summary, or NULL if the map was missing. */
const struct mem_info *meminfo_get(void)
{
	return ready ? &info : NULL;
}

/** Convert bytes to a whole-mebibyte figure for the shell. */
static uint64_t to_mib(uint64_t bytes)
{
	return bytes / (1024ull * 1024ull);
}

/** Print physical memory as seen by the bootloader. */
void meminfo_print(void)
{
	tty_set_color(TTY_COL_FG);
	tty_puts("Physical memory\n");
	tty_set_color(TTY_COL_DIM);
	if (!ready) {
		tty_puts("map: unavailable\n");
		tty_set_color(TTY_COL_FG);
		return;
	}
	tty_printf("usable: %llu MiB\n", (unsigned long long)to_mib(info.usable_bytes));
	tty_printf("total:  %llu MiB\n", (unsigned long long)to_mib(info.total_bytes));
	tty_printf("entries: %llu\n", (unsigned long long)info.entry_count);
	tty_set_color(TTY_COL_FG);
}

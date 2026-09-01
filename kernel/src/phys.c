#include "phys.h"
#include "klib.h"

#include <stdint.h>

static uint64_t hhdm;
static uint8_t *pool_virt;
static uint32_t pool_phys;
static size_t pool_size;
static size_t pool_used;

/** Round `n` up to 128 bytes (HDA BDL / CORB alignment). */
static size_t align128(size_t n)
{
	return (n + 127u) & ~(size_t)127u;
}

/**
 * Pick the first usable region below 4 GiB that can hold a 2 MiB DMA pool.
 * AC97 / HDA descriptors must live in 32-bit physical space.
 */
void phys_init(uint64_t hhdm_offset, struct limine_memmap_response *map)
{
	hhdm = hhdm_offset;
	pool_virt = NULL;
	pool_phys = 0;
	pool_size = 0;
	pool_used = 0;
	if (map == NULL) {
		return;
	}
	const size_t want = 2u * 1024u * 1024u;
	for (uint64_t i = 0; i < map->entry_count; i++) {
		struct limine_memmap_entry *e = map->entries[i];
		if (e->type != LIMINE_MEMMAP_USABLE) {
			continue;
		}
		uint64_t base = (e->base + 127ull) & ~127ull;
		uint64_t end = e->base + e->length;
		if (base >= end) {
			continue;
		}
		if (base >= 0x100000000ull) {
			continue;
		}
		uint64_t cap = end - base;
		if (base + cap > 0x100000000ull) {
			cap = 0x100000000ull - base;
		}
		if (cap < want) {
			continue;
		}
		pool_phys = (uint32_t)base;
		pool_size = want;
		pool_virt = (uint8_t *)(hhdm + base);
		memset(pool_virt, 0, pool_size);
		return;
	}
}

/** Higher-half direct map offset. */
uint64_t phys_hhdm(void)
{
	return hhdm;
}

/** Convert a physical address into an HHDM pointer. */
void *phys_to_virt(uint64_t phys)
{
	return (void *)(hhdm + phys);
}

/** Allocate zeroed DMA memory below 4 GiB. */
void *phys_alloc(size_t bytes, uint32_t *phys_out)
{
	bytes = align128(bytes);
	if (pool_virt == NULL || pool_used + bytes > pool_size) {
		if (phys_out) {
			*phys_out = 0;
		}
		return NULL;
	}
	void *v = pool_virt + pool_used;
	if (phys_out) {
		*phys_out = pool_phys + (uint32_t)pool_used;
	}
	pool_used += bytes;
	return v;
}

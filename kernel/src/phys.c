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

#define PTE_P		0x001ull
#define PTE_W		0x002ull
#define PTE_PWT		0x008ull
#define PTE_PCD		0x010ull
#define PTE_PS		0x080ull
#define PTE_ADDR	0x000ffffffffff000ull

/** Next 4 KiB-aligned offset in the DMA pool. */
static size_t align4k(size_t n)
{
	return (n + 4095u) & ~(size_t)4095u;
}

/** Allocate one page-table page (zeroed, 4 KiB aligned). */
static uint64_t alloc_pt_phys(void)
{
	pool_used = align4k(pool_used);
	if (pool_virt == NULL || pool_used + 4096u > pool_size) {
		return 0;
	}
	uint32_t phys = pool_phys + (uint32_t)pool_used;
	memset(pool_virt + pool_used, 0, 4096);
	pool_used += 4096u;
	return phys;
}

/** Page-table slot for `virt` at paging `shift`. */
static unsigned pt_index(uint64_t virt, unsigned shift)
{
	return (unsigned)((virt >> shift) & 0x1FFull);
}

/**
 * Ensure a 4 KiB uncacheable mapping of `phys` at HHDM+phys.
 * Creates page tables from the DMA pool as needed.
 */
static int map_mmio_page(uint64_t pml4_phys, uint64_t phys)
{
	uint64_t virt = hhdm + phys;
	uint64_t *pml4 = (uint64_t *)(hhdm + (pml4_phys & PTE_ADDR));
	unsigned i4 = pt_index(virt, 39);
	if ((pml4[i4] & PTE_P) == 0) {
		uint64_t n = alloc_pt_phys();
		if (n == 0) {
			return 0;
		}
		pml4[i4] = n | PTE_P | PTE_W;
	}
	uint64_t *pdpt = (uint64_t *)(hhdm + (pml4[i4] & PTE_ADDR));
	unsigned i3 = pt_index(virt, 30);
	if ((pdpt[i3] & PTE_P) == 0) {
		uint64_t n = alloc_pt_phys();
		if (n == 0) {
			return 0;
		}
		pdpt[i3] = n | PTE_P | PTE_W;
	}
	if (pdpt[i3] & PTE_PS) {
		return 0;	/* 1 GiB page in the way */
	}
	uint64_t *pd = (uint64_t *)(hhdm + (pdpt[i3] & PTE_ADDR));
	unsigned i2 = pt_index(virt, 21);
	if ((pd[i2] & PTE_P) == 0) {
		uint64_t n = alloc_pt_phys();
		if (n == 0) {
			return 0;
		}
		pd[i2] = n | PTE_P | PTE_W;
	}
	if (pd[i2] & PTE_PS) {
		return 0;	/* 2 MiB page in the way */
	}
	uint64_t *pt = (uint64_t *)(hhdm + (pd[i2] & PTE_ADDR));
	unsigned i1 = pt_index(virt, 12);
	pt[i1] = (phys & PTE_ADDR) | PTE_P | PTE_W | PTE_PWT | PTE_PCD;
	__asm__ volatile ("invlpg (%0)" : : "r"(virt) : "memory");
	return 1;
}

/**
 * Map `bytes` of MMIO at `phys` into the HHDM as uncacheable pages.
 * Needed because Limine base revision 3+ does not map PCI BARs.
 */
bool phys_map_mmio(uint64_t phys, size_t bytes)
{
	if (hhdm == 0 || bytes == 0) {
		return false;
	}
	uint64_t cr3;
	__asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));
	uint64_t start = phys & ~0xFFFull;
	uint64_t end = (phys + bytes + 0xFFFull) & ~0xFFFull;
	for (uint64_t p = start; p < end; p += 4096ull) {
		if (!map_mmio_page(cr3, p)) {
			return false;
		}
	}
	return true;
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

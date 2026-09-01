#ifndef AUDIOS_PHYS_H
#define AUDIOS_PHYS_H

#include <limine.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** Bind the HHDM and steal a low usable region for 32-bit DMA. */
void phys_init(uint64_t hhdm_offset, struct limine_memmap_response *map);

/** Allocate zeroed DMA memory below 4 GiB. Returns virt, writes phys. */
void *phys_alloc(size_t bytes, uint32_t *phys_out);

/** Higher-half direct map offset. */
uint64_t phys_hhdm(void);

/** Convert a physical address into an HHDM pointer. */
void *phys_to_virt(uint64_t phys);

/**
 * Map `bytes` of MMIO at `phys` into the HHDM as uncacheable pages.
 * Limine's HHDM does not include PCI BARs at base revision 3+.
 */
bool phys_map_mmio(uint64_t phys, size_t bytes);

#endif

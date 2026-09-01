#ifndef AUDIOS_PCI_H
#define AUDIOS_PCI_H

#include <stddef.h>
#include <stdint.h>

#define PCI_MAX_DEVICES	32
#define PCI_CLASS_AUDIO	0x04

struct pci_device {
	uint8_t bus;
	uint8_t slot;
	uint8_t func;
	uint16_t vendor;
	uint16_t device;
	uint8_t class_code;
	uint8_t subclass;
	uint8_t prog_if;
	uint8_t irq_line;
	uint32_t bar[6];
};

/** Walk the PCI bus and cache multimedia-class plus known audio IDs. */
void pci_init(void);

/** Number of cached devices. */
unsigned pci_device_count(void);

/** Return cached device `index`, or NULL. */
const struct pci_device *pci_device_at(unsigned index);

uint32_t pci_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);
uint16_t pci_read16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);
uint8_t pci_read8(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);
void pci_write16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint16_t value);
void pci_write32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t value);

/** Enable I/O space and bus mastering on a function. */
void pci_enable_io_bm(const struct pci_device *dev);

/** Enable memory space and bus mastering (HDA MMIO). */
void pci_enable_mem_bm(const struct pci_device *dev);

/** I/O port address stored in a BAR (low bit 1). */
uint16_t pci_io_bar(uint32_t bar);

/** Physical MMIO address of BAR `index` (0 if the BAR is I/O). */
uint64_t pci_mmio_bar(const struct pci_device *dev, unsigned index);

/**
 * Find the `nth` (0-based) PCI function with this class/subclass.
 * `prog_if` of 0xFF matches any programming interface.
 * Returns 1 and fills `out` on success.
 */
int pci_find_class(uint8_t class_code, uint8_t subclass, uint8_t prog_if,
	unsigned nth, struct pci_device *out);

#endif

#ifndef AUDIOS_PCI_H
#define AUDIOS_PCI_H

#include <stdint.h>

#define PCI_MAX_DEVICES	32

struct pci_device {
	uint8_t bus, slot, func;
	uint16_t vendor, device;
	uint8_t class_code, subclass;
	uint32_t bar[6];
};

/** Scan buses 0–7 for VIA/ICH AC97 (class 04 or known IDs). */
void pci_init(void);
unsigned pci_device_count(void);
const struct pci_device *pci_device_at(unsigned index);
uint32_t pci_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);
uint16_t pci_read16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);
void pci_write16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint16_t value);
void pci_write32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t value);
/** Enable I/O space + bus mastering on this function. */
void pci_enable_io_bm(const struct pci_device *dev);
/** I/O port from a BAR, or 0 if the BAR is memory-mapped. */
uint16_t pci_io_bar(uint32_t bar);

#endif

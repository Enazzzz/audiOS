#include "pci.h"
#include "io.h"

#define PCI_CONFIG_ADDR	0xCF8
#define PCI_CONFIG_DATA	0xCFC

static struct pci_device devices[PCI_MAX_DEVICES];
static unsigned device_count;

/** Build a type-1 configuration address. */
static uint32_t pci_addr(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset)
{
	return 0x80000000u
		| ((uint32_t)bus << 16)
		| ((uint32_t)slot << 11)
		| ((uint32_t)func << 8)
		| (offset & 0xFC);
}

/** Read a 32-bit PCI config dword. */
uint32_t pci_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset)
{
	outl(PCI_CONFIG_ADDR, pci_addr(bus, slot, func, offset));
	return inl(PCI_CONFIG_DATA);
}

/** Write a 32-bit PCI config dword. */
void pci_write32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t value)
{
	outl(PCI_CONFIG_ADDR, pci_addr(bus, slot, func, offset));
	outl(PCI_CONFIG_DATA, value);
}

/** Read a 16-bit PCI config word. */
uint16_t pci_read16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset)
{
	uint32_t v = pci_read32(bus, slot, func, offset);
	return (uint16_t)(v >> ((offset & 2) * 8));
}

/** Read an 8-bit PCI config byte. */
uint8_t pci_read8(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset)
{
	uint32_t v = pci_read32(bus, slot, func, offset);
	return (uint8_t)(v >> ((offset & 3) * 8));
}

/** Write a 16-bit PCI config word without touching the sibling bytes. */
void pci_write16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint16_t value)
{
	uint32_t old = pci_read32(bus, slot, func, offset);
	uint32_t shift = (offset & 2) * 8;
	uint32_t mask = 0xFFFFu << shift;
	pci_write32(bus, slot, func, offset, (old & ~mask) | ((uint32_t)value << shift));
}

/** True if this function looks like an audio endpoint we should advertise. */
static int pci_is_audio(uint16_t vendor, uint16_t device, uint8_t class_code, uint8_t subclass)
{
	(void)subclass;
	if (class_code == PCI_CLASS_AUDIO) {
		return 1;
	}
	/* Intel ICH AC97, even if class is reported oddly. */
	if (vendor == 0x8086 && device == 0x2415) {
		return 1;
	}
	return 0;
}

/** Record one function into the audio device table. */
static void pci_store(uint8_t bus, uint8_t slot, uint8_t func,
	uint16_t vendor, uint16_t device)
{
	if (device_count >= PCI_MAX_DEVICES) {
		return;
	}
	struct pci_device *d = &devices[device_count++];
	d->bus = bus;
	d->slot = slot;
	d->func = func;
	d->vendor = vendor;
	d->device = device;
	uint32_t classreg = pci_read32(bus, slot, func, 0x08);
	d->class_code = (uint8_t)(classreg >> 24);
	d->subclass = (uint8_t)(classreg >> 16);
	d->prog_if = (uint8_t)(classreg >> 8);
	d->irq_line = pci_read8(bus, slot, func, 0x3C);
	for (int i = 0; i < 6; i++) {
		d->bar[i] = pci_read32(bus, slot, func, (uint8_t)(0x10 + i * 4));
	}
}

/** Scan a handful of buses for audio-class devices. */
void pci_init(void)
{
	device_count = 0;
	for (uint16_t bus = 0; bus < 8; bus++) {
		for (uint8_t slot = 0; slot < 32; slot++) {
			uint16_t vendor = pci_read16((uint8_t)bus, slot, 0, 0x00);
			if (vendor == 0xFFFF) {
				continue;
			}
			uint8_t header = pci_read8((uint8_t)bus, slot, 0, 0x0E);
			uint8_t funcs = (header & 0x80) ? 8 : 1;
			for (uint8_t func = 0; func < funcs; func++) {
				vendor = pci_read16((uint8_t)bus, slot, func, 0x00);
				if (vendor == 0xFFFF) {
					continue;
				}
				uint16_t device = pci_read16((uint8_t)bus, slot, func, 0x02);
				uint32_t classreg = pci_read32((uint8_t)bus, slot, func, 0x08);
				uint8_t class_code = (uint8_t)(classreg >> 24);
				uint8_t subclass = (uint8_t)(classreg >> 16);
				if (pci_is_audio(vendor, device, class_code, subclass)) {
					pci_store((uint8_t)bus, slot, func, vendor, device);
				}
			}
		}
	}
}

/** Number of cached devices. */
unsigned pci_device_count(void)
{
	return device_count;
}

/** Return cached device `index`, or NULL. */
const struct pci_device *pci_device_at(unsigned index)
{
	if (index >= device_count) {
		return NULL;
	}
	return &devices[index];
}

/** Enable I/O space and bus mastering on a function. */
void pci_enable_io_bm(const struct pci_device *dev)
{
	uint16_t cmd = pci_read16(dev->bus, dev->slot, dev->func, 0x04);
	cmd |= 0x0005;	/* IO space + bus master */
	pci_write16(dev->bus, dev->slot, dev->func, 0x04, cmd);
}

/** Enable memory space and bus mastering (needed for HDA). */
void pci_enable_mem_bm(const struct pci_device *dev)
{
	uint16_t cmd = pci_read16(dev->bus, dev->slot, dev->func, 0x04);
	cmd |= 0x0006;	/* memory space + bus master */
	pci_write16(dev->bus, dev->slot, dev->func, 0x04, cmd);
}

/** Physical MMIO address of BAR `index` (0 if the BAR is I/O). */
uint64_t pci_mmio_bar(const struct pci_device *dev, unsigned index)
{
	if (index > 5) {
		return 0;
	}
	uint32_t bar = dev->bar[index];
	if (bar & 1u) {
		return 0;
	}
	uint64_t addr = (uint64_t)(bar & ~0xFu);
	if ((bar & 0x6u) == 0x4u && index + 1u <= 5u) {
		addr |= (uint64_t)dev->bar[index + 1u] << 32;
	}
	return addr;
}

/** I/O port address stored in a BAR (low bit 1). */
uint16_t pci_io_bar(uint32_t bar)
{
	if ((bar & 1) == 0) {
		return 0;
	}
	return (uint16_t)(bar & ~3u);
}

/** Fill `out` from live config space. */
static void pci_fill(struct pci_device *d, uint8_t bus, uint8_t slot, uint8_t func)
{
	d->bus = bus;
	d->slot = slot;
	d->func = func;
	d->vendor = pci_read16(bus, slot, func, 0x00);
	d->device = pci_read16(bus, slot, func, 0x02);
	uint32_t classreg = pci_read32(bus, slot, func, 0x08);
	d->class_code = (uint8_t)(classreg >> 24);
	d->subclass = (uint8_t)(classreg >> 16);
	d->prog_if = (uint8_t)(classreg >> 8);
	d->irq_line = pci_read8(bus, slot, func, 0x3C);
	for (int i = 0; i < 6; i++) {
		d->bar[i] = pci_read32(bus, slot, func, (uint8_t)(0x10 + i * 4));
	}
}

int pci_find_class(uint8_t class_code, uint8_t subclass, uint8_t prog_if,
	unsigned nth, struct pci_device *out)
{
	unsigned seen = 0;
	for (uint16_t bus = 0; bus < 8; bus++) {
		for (uint8_t slot = 0; slot < 32; slot++) {
			uint16_t vendor = pci_read16((uint8_t)bus, slot, 0, 0x00);
			if (vendor == 0xFFFF) {
				continue;
			}
			uint8_t header = pci_read8((uint8_t)bus, slot, 0, 0x0E);
			uint8_t funcs = (header & 0x80) ? 8 : 1;
			for (uint8_t func = 0; func < funcs; func++) {
				vendor = pci_read16((uint8_t)bus, slot, func, 0x00);
				if (vendor == 0xFFFF) {
					continue;
				}
				uint32_t classreg = pci_read32((uint8_t)bus, slot, func, 0x08);
				uint8_t cc = (uint8_t)(classreg >> 24);
				uint8_t sc = (uint8_t)(classreg >> 16);
				uint8_t pi = (uint8_t)(classreg >> 8);
				if (cc != class_code || sc != subclass) {
					continue;
				}
				if (prog_if != 0xFF && pi != prog_if) {
					continue;
				}
				if (seen == nth) {
					if (out) {
						pci_fill(out, (uint8_t)bus, slot, func);
					}
					return 1;
				}
				seen++;
			}
		}
	}
	return 0;
}

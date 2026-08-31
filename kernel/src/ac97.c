#include "ac97.h"
#include "io.h"
#include "klib.h"
#include "pci.h"
#include "phys.h"

#define AC97_PERIODS	32

/* Mixer (NAMBAR) */
#define AC97_RESET		0x00
#define AC97_MASTER_VOL		0x02
#define AC97_PCM_VOL		0x18
#define AC97_EXT_ID		0x28
#define AC97_EXT_CTRL		0x2A
#define AC97_PCM_FRONT_DAC	0x2C

/* Bus master (NABMBAR) PCM-out */
#define PO_BDBAR	0x10
#define PO_CIV		0x14
#define PO_LVI		0x15
#define PO_SR		0x16
#define PO_CR		0x1B
#define GLOB_CNT	0x2C
#define GLOB_STA	0x30

#define SR_DCH		0x01
#define SR_CELV		0x04
#define SR_LVBCI	0x08
#define SR_BCIS		0x10
#define SR_FIFOE	0x10	/* some docs; FIFO error is bit 3 on ICH */

struct ac97_bd {
	uint32_t addr;
	uint16_t samples;
	uint16_t control;
} __attribute__((packed));

static uint16_t nambar;
static uint16_t nabmbar;
static int present;
static char name[40];
static uint32_t hw_rate = 48000;
static uint32_t underruns;
static uint32_t period_frames;
static uint32_t period_bytes;
static struct ac97_bd *bdl;
static uint32_t bdl_phys;
static uint8_t *pcm;
static uint32_t pcm_phys;
static uint8_t next_fill;
static int running;
static uint8_t bound_bus;
static uint8_t bound_slot;
static uint8_t bound_func;
static uint32_t bound_index;

/** Spin for a few microseconds using port 0x80. */
static void ac97_delay(unsigned n)
{
	while (n--) {
		io_wait();
	}
}

/** True if this PCI function is a usable ICH-style AC97 controller. */
static int ac97_match(const struct pci_device *d)
{
	if (d->vendor == 0x8086 && d->device == 0x2415) {
		return 1;
	}
	return d->class_code == 0x04 && d->subclass == 0x01 && pci_io_bar(d->bar[0]) != 0;
}

/** Probe PCI for ICH AC97 and bring the codec out of reset. */
bool ac97_init(void)
{
	present = 0;
	running = 0;
	nambar = 0;
	nabmbar = 0;
	unsigned count = pci_device_count();
	const struct pci_device *dev = NULL;
	uint32_t found = UINT32_MAX;
	/* Prefer Intel ICH AC97 even if HDA appears first on the same bus. */
	for (unsigned i = 0; i < count; i++) {
		const struct pci_device *d = pci_device_at(i);
		if (d->vendor == 0x8086 && d->device == 0x2415) {
			dev = d;
			found = i;
			break;
		}
	}
	if (dev == NULL) {
		for (unsigned i = 0; i < count; i++) {
			const struct pci_device *d = pci_device_at(i);
			if (ac97_match(d)) {
				dev = d;
				found = i;
				break;
			}
		}
	}
	if (dev == NULL) {
		return false;
	}
	nambar = pci_io_bar(dev->bar[0]);
	nabmbar = pci_io_bar(dev->bar[1]);
	if (nambar == 0 || nabmbar == 0) {
		return false;
	}
	pci_enable_io_bm(dev);

	outl(nabmbar + GLOB_CNT, 0x02);	/* cold reset */
	ac97_delay(1000);
	outl(nabmbar + GLOB_CNT, 0x00);
	ac97_delay(1000);

	outw(nambar + AC97_RESET, 0);
	ac97_delay(1000);

	outw(nambar + AC97_MASTER_VOL, 0x0000);
	outw(nambar + AC97_PCM_VOL, 0x0000);

	uint16_t ext = inw(nambar + AC97_EXT_ID);
	if (ext & 0x0001) {
		outw(nambar + AC97_EXT_CTRL, 0x0001);	/* VRA */
		ac97_delay(100);
		outw(nambar + AC97_PCM_FRONT_DAC, 48000);
		ac97_delay(100);
		hw_rate = inw(nambar + AC97_PCM_FRONT_DAC);
		if (hw_rate < 8000 || hw_rate > 192000) {
			hw_rate = 48000;
		}
	} else {
		hw_rate = 48000;
	}

	bdl = phys_alloc(sizeof(struct ac97_bd) * AC97_PERIODS, &bdl_phys);
	/* Max period 256 frames stereo s16 → 1024 bytes, 32 periods → 32 KiB. */
	pcm = phys_alloc(AC97_PERIODS * 256u * 4u, &pcm_phys);
	if (bdl == NULL || pcm == NULL) {
		return false;
	}

	ksnprintf(name, sizeof(name), "Intel AC97 (%x:%x)",
		(unsigned)dev->vendor, (unsigned)dev->device);
	bound_bus = dev->bus;
	bound_slot = dev->slot;
	bound_func = dev->func;
	bound_index = found;
	present = 1;
	return true;
}

/** True after a successful probe. */
bool ac97_present(void)
{
	return present != 0;
}

/** PCI table index of the bound AC97 function, or UINT32_MAX. */
uint32_t ac97_pci_index(void)
{
	return present ? bound_index : UINT32_MAX;
}

/**
 * True if the bound function still answers PCI config.
 * A 0xFFFF vendor read means the card vanished; report it without panicking.
 */
bool ac97_alive(void)
{
	if (!present) {
		return false;
	}
	return pci_read16(bound_bus, bound_slot, bound_func, 0x00) != 0xFFFF;
}

/** Human-readable device name. */
const char *ac97_name(void)
{
	return present ? name : "none";
}

/** Hardware PCM rate in Hz. */
uint32_t ac97_hw_rate(void)
{
	return hw_rate;
}

/** Start PCM-out DMA with `period_frames` stereo frames per descriptor. */
bool ac97_start(uint32_t frames, void (*fill)(int16_t *dst, uint32_t frames))
{
	if (!present || bdl == NULL || pcm == NULL || fill == NULL) {
		return false;
	}
	if (frames < 16) {
		frames = 16;
	}
	if (frames > 256) {
		frames = 256;
	}
	period_frames = frames;
	period_bytes = frames * 4u;	/* stereo s16 */
	underruns = 0;

	outb(nabmbar + PO_CR, 0x02);
	ac97_delay(100);
	outb(nabmbar + PO_CR, 0x00);
	outw(nabmbar + PO_SR, 0x1C);

	for (unsigned i = 0; i < AC97_PERIODS; i++) {
		bdl[i].addr = pcm_phys + i * period_bytes;
		bdl[i].samples = (uint16_t)(frames * 2);
		bdl[i].control = 0x8000;
		fill((int16_t *)(pcm + i * period_bytes), frames);
	}
	next_fill = 0;

	outl(nabmbar + PO_BDBAR, bdl_phys);
	outb(nabmbar + PO_LVI, AC97_PERIODS - 1);
	outb(nabmbar + PO_CR, 0x01);
	running = 1;
	return true;
}

/** Stop PCM-out DMA. */
void ac97_stop(void)
{
	if (!present) {
		return;
	}
	outb(nabmbar + PO_CR, 0x00);
	outb(nabmbar + PO_CR, 0x02);
	ac97_delay(50);
	outb(nabmbar + PO_CR, 0x00);
	running = 0;
}

/** Hardware underrun counter since last start. */
uint32_t ac97_underruns(void)
{
	return underruns;
}

/**
 * Refill descriptors the controller has already consumed.
 * `fill` writes one period of stereo s16 into `dst`.
 */
unsigned ac97_service(void (*fill)(int16_t *dst, uint32_t frames))
{
	if (!present || !running || fill == NULL) {
		return 0;
	}
	uint16_t sr = inw(nabmbar + PO_SR);
	if (sr & 0x08) {
		underruns++;
	}
	outw(nabmbar + PO_SR, sr);

	uint8_t civ = inb(nabmbar + PO_CIV);
	unsigned filled = 0;
	while (next_fill != civ && filled < AC97_PERIODS) {
		int16_t *dst = (int16_t *)(pcm + next_fill * period_bytes);
		fill(dst, period_frames);
		next_fill = (uint8_t)((next_fill + 1) % AC97_PERIODS);
		filled++;
	}
	outb(nabmbar + PO_LVI, (uint8_t)((civ + AC97_PERIODS - 2) % AC97_PERIODS));
	if (filled == 0 && (sr & SR_DCH)) {
		underruns++;
		outb(nabmbar + PO_CR, 0x01);
	}
	return filled;
}

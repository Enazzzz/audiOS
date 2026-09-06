#include "audio.h"
#include "io.h"
#include "klib.h"
#include "pci.h"

#include <stdint.h>

#define PERIODS		8
#define PERIOD_FR	48u
#define PERIOD_BYTES	(PERIOD_FR * 4u)

/* ICH AC97 */
#define AC97_RESET	0x00
#define AC97_MASTER	0x02
#define AC97_PCM_VOL	0x18
#define AC97_REC_SEL	0x1A
#define AC97_REC_GAIN	0x1C
#define PO_BDBAR	0x10
#define PO_CIV		0x14
#define PO_LVI		0x15
#define PO_SR		0x16
#define PO_CR		0x1B
#define PI_BDBAR	0x00
#define PI_CIV		0x04
#define PI_LVI		0x05
#define PI_SR		0x06
#define PI_CR		0x0B
#define GLOB_CNT	0x2C

/* VIA VT8233 */
#define VIA_AC97	0x80
#define VIA_PLAY	0x00
#define VIA_MC		0x40
#define VIA_CAP		0x60
#define VIA_CTRL_START	0x80
#define VIA_CTRL_TERM	0x40
#define VIA_CTRL_RESET	0x01
#define VIA_EOL		(1u << 31)
#define VIA_FLAG	(1u << 30)

struct ac97_bd {
	uint32_t addr;
	uint16_t samples;
	uint16_t control;
} __attribute__((packed));

struct via_sgd {
	uint32_t addr;
	uint32_t size;
} __attribute__((packed));

static int present;
static int via_mode;		/* 1 = VIA SGD, 0 = ICH */
static int via8233a;
static char name[48];
static uint16_t nambar, nabmbar, via_base;

static uint8_t *heap = (uint8_t *)0x200000;
static uint32_t heap_used;

static struct ac97_bd *ich_po_bdl;
static struct ac97_bd *ich_pi_bdl;
static uint32_t ich_po_bdl_phys, ich_pi_bdl_phys;
static uint8_t *po_pcm;
static uint8_t *pi_pcm;
static uint32_t po_pcm_phys, pi_pcm_phys;
static struct via_sgd *via_po;
static struct via_sgd *via_pi;
static uint32_t via_po_phys, via_pi_phys;
static uint8_t next_po;
static uint8_t next_pi;
static int running;
static void (*fill_fn)(int16_t *, uint32_t);
static void (*cap_fn)(const int16_t *, uint32_t);

/** Identity-mapped DMA bump allocator (above 2 MiB, below 16 MiB). */
static void *dma_alloc(uint32_t bytes, uint32_t *phys)
{
	bytes = (bytes + 15u) & ~15u;
	void *p = heap + heap_used;
	if (phys) {
		*phys = 0x200000u + heap_used;
	}
	memset(p, 0, bytes);
	heap_used += bytes;
	return p;
}

static void delay(unsigned n)
{
	while (n--) {
		io_wait();
	}
}

/** VIA AC97 codec write. */
static void via_codec_write(uint8_t reg, uint16_t val)
{
	unsigned t;
	outl(via_base + VIA_AC97, ((uint32_t)reg << 16) | val);
	for (t = 0; t < 10000; t++) {
		if ((inl(via_base + VIA_AC97) & (1u << 24)) == 0) {
			return;
		}
		io_wait();
	}
}

static uint16_t via_codec_read(uint8_t reg)
{
	unsigned t;
	outl(via_base + VIA_AC97, (1u << 23) | ((uint32_t)reg << 16));
	for (t = 0; t < 10000; t++) {
		uint32_t v = inl(via_base + VIA_AC97);
		if ((v & (1u << 24)) == 0) {
			return (uint16_t)v;
		}
		io_wait();
	}
	return 0xFFFF;
}

static void ich_codec_write(uint8_t reg, uint16_t val)
{
	outw(nambar + reg, val);
	delay(50);
}

static int probe_via(const struct pci_device *d)
{
	uint8_t rev;
	via_base = pci_io_bar(d->bar[0]);
	if (via_base == 0) {
		return 0;
	}
	pci_enable_io_bm(d);
	rev = (uint8_t)pci_read32(d->bus, d->slot, d->func, 0x08);
	via8233a = (d->device == 0x3059 && rev >= 0x20);
	via_codec_write(AC97_RESET, 0);
	delay(2000);
	via_codec_write(AC97_MASTER, 0);
	via_codec_write(AC97_PCM_VOL, 0);
	via_codec_write(AC97_REC_SEL, 0x0404);	/* line-in */
	via_codec_write(AC97_REC_GAIN, 0);
	(void)via_codec_read(AC97_MASTER);
	ksnprintf(name, sizeof(name), "VIA %s (%x:%x rev %u)",
		via8233a ? "VT8233A" : "VT82xx",
		(unsigned)d->vendor, (unsigned)d->device, (unsigned)rev);
	via_mode = 1;
	return 1;
}

static int probe_ich(const struct pci_device *d)
{
	nambar = pci_io_bar(d->bar[0]);
	nabmbar = pci_io_bar(d->bar[1]);
	if (nambar == 0 || nabmbar == 0) {
		return 0;
	}
	pci_enable_io_bm(d);
	outl(nabmbar + GLOB_CNT, 0x02);
	delay(1000);
	outl(nabmbar + GLOB_CNT, 0x00);
	delay(1000);
	ich_codec_write(AC97_RESET, 0);
	ich_codec_write(AC97_MASTER, 0);
	ich_codec_write(AC97_PCM_VOL, 0);
	ich_codec_write(AC97_REC_SEL, 0x0404);
	ich_codec_write(AC97_REC_GAIN, 0);
	ksnprintf(name, sizeof(name), "ICH AC97 (%x:%x)",
		(unsigned)d->vendor, (unsigned)d->device);
	via_mode = 0;
	return 1;
}

void audio_init(void)
{
	unsigned i, n;
	present = 0;
	heap_used = 0;
	n = pci_device_count();
	for (i = 0; i < n; i++) {
		const struct pci_device *d = pci_device_at(i);
		if (d->vendor == 0x1106 && (d->device == 0x3058 || d->device == 0x3059)) {
			if (probe_via(d)) {
				present = 1;
				return;
			}
		}
	}
	for (i = 0; i < n; i++) {
		const struct pci_device *d = pci_device_at(i);
		if (probe_ich(d)) {
			present = 1;
			return;
		}
	}
	ksnprintf(name, sizeof(name), "%s", "none (software loopback only)");
}

int audio_present(void)
{
	return present;
}

const char *audio_name(void)
{
	return name;
}

/** Program a VIA SGD channel: table pointer, 16-bit stereo, start. */
static void via_start_ch(uint16_t base, uint32_t table_phys, int playback)
{
	outb(base + 1, VIA_CTRL_TERM);
	delay(20);
	outb(base + 1, VIA_CTRL_RESET);
	delay(20);
	outb(base + 1, 0);
	outl(base + 4, table_phys);
	if (playback) {
		/* 16-bit stereo; 8233A multi-channel format at +2. */
		outb(base + 2, (uint8_t)(0x80 | 0x20));
		if (via8233a) {
			outb(base + 2, 0x90);	/* 16-bit, 2ch */
		}
	}
	outl(base + 8, 0xFF000000u);
	outb(base + 1, VIA_CTRL_START);
}

int audio_start_link(void (*fill)(int16_t *dst, uint32_t frames),
	void (*cap)(const int16_t *src, uint32_t frames))
{
	unsigned i;
	if (!present || fill == NULL) {
		return 0;
	}
	fill_fn = fill;
	cap_fn = cap;
	po_pcm = dma_alloc(PERIODS * PERIOD_BYTES, &po_pcm_phys);
	pi_pcm = dma_alloc(PERIODS * PERIOD_BYTES, &pi_pcm_phys);
	next_po = 0;
	next_pi = 0;
	for (i = 0; i < PERIODS; i++) {
		fill((int16_t *)(po_pcm + i * PERIOD_BYTES), PERIOD_FR);
	}
	if (via_mode) {
		via_po = dma_alloc(sizeof(struct via_sgd) * PERIODS, &via_po_phys);
		via_pi = dma_alloc(sizeof(struct via_sgd) * PERIODS, &via_pi_phys);
		for (i = 0; i < PERIODS; i++) {
			via_po[i].addr = po_pcm_phys + i * PERIOD_BYTES;
			via_po[i].size = PERIOD_BYTES | VIA_FLAG;
			via_pi[i].addr = pi_pcm_phys + i * PERIOD_BYTES;
			via_pi[i].size = PERIOD_BYTES | VIA_FLAG;
		}
		via_po[PERIODS - 1].size |= VIA_EOL;
		via_pi[PERIODS - 1].size |= VIA_EOL;
		via_start_ch((uint16_t)(via_base + (via8233a ? VIA_MC : VIA_PLAY)),
			via_po_phys, 1);
		via_start_ch((uint16_t)(via_base + VIA_CAP), via_pi_phys, 0);
	} else {
		ich_po_bdl = dma_alloc(sizeof(struct ac97_bd) * PERIODS, &ich_po_bdl_phys);
		ich_pi_bdl = dma_alloc(sizeof(struct ac97_bd) * PERIODS, &ich_pi_bdl_phys);
		for (i = 0; i < PERIODS; i++) {
			ich_po_bdl[i].addr = po_pcm_phys + i * PERIOD_BYTES;
			ich_po_bdl[i].samples = (uint16_t)(PERIOD_FR * 2);
			ich_po_bdl[i].control = 0x8000;
			ich_pi_bdl[i].addr = pi_pcm_phys + i * PERIOD_BYTES;
			ich_pi_bdl[i].samples = (uint16_t)(PERIOD_FR * 2);
			ich_pi_bdl[i].control = 0x8000;
		}
		outb(nabmbar + PO_CR, 0x02);
		outb(nabmbar + PI_CR, 0x02);
		delay(50);
		outl(nabmbar + PO_BDBAR, ich_po_bdl_phys);
		outb(nabmbar + PO_LVI, PERIODS - 1);
		outl(nabmbar + PI_BDBAR, ich_pi_bdl_phys);
		outb(nabmbar + PI_LVI, PERIODS - 1);
		outb(nabmbar + PO_CR, 0x01);
		outb(nabmbar + PI_CR, 0x01);
	}
	running = 1;
	return 1;
}

void audio_stop(void)
{
	if (!present || !running) {
		running = 0;
		return;
	}
	if (via_mode) {
		outb((uint16_t)(via_base + VIA_PLAY + 1), VIA_CTRL_TERM);
		outb((uint16_t)(via_base + VIA_MC + 1), VIA_CTRL_TERM);
		outb((uint16_t)(via_base + VIA_CAP + 1), VIA_CTRL_TERM);
	} else if (nabmbar) {
		outb(nabmbar + PO_CR, 0);
		outb(nabmbar + PI_CR, 0);
	}
	running = 0;
}

void audio_service(void)
{
	unsigned i;
	if (!present || !running || fill_fn == NULL) {
		return;
	}
	if (!via_mode) {
		uint8_t civ = inb(nabmbar + PO_CIV);
		while (next_po != civ) {
			fill_fn((int16_t *)(po_pcm + next_po * PERIOD_BYTES), PERIOD_FR);
			next_po = (uint8_t)((next_po + 1) % PERIODS);
		}
		if (cap_fn) {
			uint8_t iciv = inb(nabmbar + PI_CIV);
			while (next_pi != iciv) {
				cap_fn((const int16_t *)(pi_pcm + next_pi * PERIOD_BYTES), PERIOD_FR);
				next_pi = (uint8_t)((next_pi + 1) % PERIODS);
			}
		}
		outb(nabmbar + PO_LVI, (uint8_t)((civ + PERIODS - 2) % PERIODS));
		outb(nabmbar + PI_LVI, (uint8_t)((inb(nabmbar + PI_CIV) + PERIODS - 2) % PERIODS));
		return;
	}
	/* VIA: walk FLAG/EOL by counting periods against a software cursor.
	 * The current pointer register is at +0x0C (count) / +0x04 (ptr). */
	{
		uint16_t pbase = (uint16_t)(via_base + (via8233a ? VIA_MC : VIA_PLAY));
		uint32_t ptr = inl(pbase + 4);
		unsigned hw = 0;
		if (ptr >= via_po_phys) {
			hw = (unsigned)((ptr - via_po_phys) / sizeof(struct via_sgd)) % PERIODS;
		}
		for (i = 0; i < PERIODS && next_po != (uint8_t)hw; i++) {
			fill_fn((int16_t *)(po_pcm + next_po * PERIOD_BYTES), PERIOD_FR);
			next_po = (uint8_t)((next_po + 1) % PERIODS);
		}
		if (cap_fn) {
			uint32_t cptr = inl((uint16_t)(via_base + VIA_CAP + 4));
			unsigned chw = 0;
			if (cptr >= via_pi_phys) {
				chw = (unsigned)((cptr - via_pi_phys) / sizeof(struct via_sgd)) % PERIODS;
			}
			for (i = 0; i < PERIODS && next_pi != (uint8_t)chw; i++) {
				cap_fn((const int16_t *)(pi_pcm + next_pi * PERIOD_BYTES), PERIOD_FR);
				next_pi = (uint8_t)((next_pi + 1) % PERIODS);
			}
		}
	}
}

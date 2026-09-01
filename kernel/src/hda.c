#include "hda.h"
#include "io.h"
#include "klib.h"
#include "pci.h"
#include "phys.h"
#include "pit.h"
#include "tty.h"

#define HDA_PERIODS		32
#define HDA_STREAM_TAG		1
#define HDA_FMT_48K_S16_2CH	0x0011
#define HDA_CORB_ENTRIES	256

/* Controller registers */
#define HDA_GCAP	0x00
#define HDA_GCTL	0x08
#define HDA_WAKEEN	0x0C
#define HDA_STATESTS	0x0E
#define HDA_INTCTL	0x20
#define HDA_INTSTS	0x24
#define HDA_CORBLBASE	0x40
#define HDA_CORBUBASE	0x44
#define HDA_CORBWP	0x48
#define HDA_CORBRP	0x4A
#define HDA_CORBCTL	0x4C
#define HDA_CORBSIZE	0x4E
#define HDA_RIRBLBASE	0x50
#define HDA_RIRBUBASE	0x54
#define HDA_RIRBWP	0x58
#define HDA_RINTCNT	0x5A
#define HDA_RIRBCTL	0x5C
#define HDA_RIRBSTS	0x5D
#define HDA_RIRBSIZE	0x5E
#define HDA_IC		0x60
#define HDA_IR		0x64
#define HDA_ICS		0x68
#define ICS_BUSY	0x01
#define ICS_VALID	0x02

#define GCTL_CRST	0x01
#define CORBCTL_RUN	0x02
#define RIRBCTL_DMA	0x02
#define RIRBCTL_OIE	0x04

/* Stream descriptor, relative to sd_off */
#define SD_CTL		0x00
#define SD_STS		0x03
#define SD_LPIB		0x04
#define SD_CBL		0x08
#define SD_LVI		0x0C
#define SD_FMT		0x12
#define SD_BDPL		0x18
#define SD_BDPU		0x1C

#define SD_CTL_SRST	0x01
#define SD_CTL_RUN	0x02
#define SD_STS_BCIS	0x04
#define SD_STS_FIFOE	0x08
#define SD_STS_DESE	0x10

/* Verbs */
#define VERB_GET_PARAM		0xF00
#define VERB_GET_CONN_SEL	0xF01
#define VERB_GET_CONN_LIST	0xF02
#define VERB_SET_POWER		0x705
#define VERB_SET_STREAM_CHAN	0x706
#define VERB_SET_PIN_CTRL	0x707
#define VERB_SET_EAPD		0x70C
#define VERB_GET_CONFIG		0xF1C
#define VERB_GET_PIN_CTRL	0xF07
#define VERB_SET_CONV_FMT	0x2
#define VERB_GET_AMP		0xB
#define VERB_SET_AMP		0x3

#define PARAM_VENDOR_ID		0x00
#define PARAM_SUB_NODE		0x04
#define PARAM_FNG_TYPE		0x05
#define PARAM_WIDGET_CAPS	0x09
#define PARAM_PIN_CAPS		0x0C
#define PARAM_CONN_LIST_LEN	0x0E
#define PARAM_AMP_OUT_CAP	0x12
#define PARAM_AMP_IN_CAP	0x0D

#define WCAP_TYPE_SHIFT		20
#define WCAP_TYPE_MASK		0xF
#define WCAP_OUT_AMP		(1u << 2)
#define WCAP_IN_AMP		(1u << 1)
#define WCAP_CONN_LIST		(1u << 8)

#define WIDGET_DAC		0
#define WIDGET_MIXER		2
#define WIDGET_PIN		4

#define PIN_DEV_LINE_OUT	0
#define PIN_DEV_SPEAKER		1
#define PIN_DEV_HP		2

#define REALTEK_ALC662		0x10EC0662u
#define AMD_SB710_HDA_VEN	0x1002
#define AMD_SB710_HDA_DEV	0x4383

struct hda_bdl {
	uint64_t addr;
	uint32_t len;
	uint32_t ioc;
} __attribute__((packed));

static volatile uint8_t *mmio;
static uint32_t sd_off;
static int present;
static char name[56];
static uint8_t bound_bus;
static uint8_t bound_slot;
static uint8_t bound_func;
static uint32_t bound_index;
static uint8_t cad;
static uint8_t nid_afg;
static uint8_t nid_dac;
static uint8_t nid_pin;
static uint32_t codec_vid;
static uint32_t *corb;
static uint32_t corb_phys;
static uint64_t *rirb;
static uint32_t rirb_phys;
static uint16_t rirb_rp;
static uint16_t corb_mask;
static int use_corb;
static struct hda_bdl *bdl;
static uint32_t bdl_phys;
static uint8_t *pcm;
static uint32_t pcm_phys;
static uint32_t period_frames;
static uint32_t period_bytes;
static uint8_t next_fill;
static int running;
static uint32_t underruns;
static uint32_t hw_rate = 48000;

/** Spin on port 0x80 for a short delay. */
static void hda_delay(unsigned n)
{
	while (n--) {
		io_wait();
	}
}

/** Sleep `ms` using the calibrated TSC (works before IRQs are on). */
static void hda_msleep(uint32_t ms)
{
	uint64_t start = pit_ticks();
	while (pit_ticks() - start < (uint64_t)ms) {
		__asm__ volatile ("pause");
	}
}

/**
 * ATI/AMD SB450–SB710 need PCI 0x42 snoop enabled or CORB/stream DMA
 * reads stale cache lines.
 */
static void ati_enable_snoop(const struct pci_device *dev)
{
	if (dev->vendor != AMD_SB710_HDA_VEN) {
		return;
	}
	uint32_t v = pci_read32(dev->bus, dev->slot, dev->func, 0x40);
	v = (v & ~0x070000u) | 0x020000u;
	pci_write32(dev->bus, dev->slot, dev->func, 0x40, v);
}

static uint8_t mmr8(uint32_t off)
{
	return *(volatile uint8_t *)(mmio + off);
}

static uint16_t mmr16(uint32_t off)
{
	return *(volatile uint16_t *)(mmio + off);
}

static uint32_t mmr32(uint32_t off)
{
	return *(volatile uint32_t *)(mmio + off);
}

static void mmw8(uint32_t off, uint8_t v)
{
	*(volatile uint8_t *)(mmio + off) = v;
}

static void mmw16(uint32_t off, uint16_t v)
{
	*(volatile uint16_t *)(mmio + off) = v;
}

static void mmw32(uint32_t off, uint32_t v)
{
	*(volatile uint32_t *)(mmio + off) = v;
}

/** Pack a codec verb for CORB. */
static uint32_t make_verb(uint8_t node, uint32_t verb, uint32_t payload)
{
	uint32_t w = ((uint32_t)cad << 28) | ((uint32_t)node << 20);
	if (verb <= 0xFu) {
		return w | (verb << 16) | (payload & 0xFFFFu);
	}
	return w | (verb << 8) | (payload & 0xFFu);
}

/** Immediate-command path. QEMU ICH9 answers this; SB710 often does not. */
static uint32_t ics_cmd(uint32_t w)
{
	uint64_t deadline = pit_ticks() + 100;
	while ((mmr16(HDA_ICS) & ICS_BUSY) != 0 && pit_ticks() < deadline) {
		__asm__ volatile ("pause");
	}
	mmw16(HDA_ICS, ICS_VALID);
	mmw32(HDA_IC, w);
	mmw16(HDA_ICS, ICS_BUSY);
	deadline = pit_ticks() + 100;
	while (pit_ticks() < deadline) {
		uint16_t ics = mmr16(HDA_ICS);
		if ((ics & ICS_BUSY) == 0) {
			if (ics & ICS_VALID) {
				return mmr32(HDA_IR);
			}
			break;
		}
		__asm__ volatile ("pause");
	}
	return 0xFFFFFFFFu;
}

/** CORB/RIRB path. This is what Linux uses on the SB710. */
static uint32_t corb_send(uint32_t w)
{
	uint16_t wp = mmr16(HDA_CORBWP) & corb_mask;
	uint16_t next = (uint16_t)((wp + 1u) & corb_mask);
	corb[next] = w;
	__asm__ volatile ("mfence" ::: "memory");
	mmw16(HDA_CORBWP, next);

	uint64_t deadline = pit_ticks() + 100;
	while (pit_ticks() < deadline) {
		uint16_t rwp = mmr16(HDA_RIRBWP) & corb_mask;
		if (rwp != rirb_rp) {
			rirb_rp = (uint16_t)((rirb_rp + 1u) & corb_mask);
			mmw8(HDA_RIRBSTS, 0x05);
			return (uint32_t)rirb[rirb_rp];
		}
		__asm__ volatile ("pause");
	}
	return 0xFFFFFFFFu;
}

/**
 * Issue one verb. Prefer CORB on the FX board; ICS is the QEMU fallback.
 */
static uint32_t corb_cmd(uint8_t node, uint32_t verb, uint32_t payload)
{
	uint32_t w = make_verb(node, verb, payload);
	if (use_corb) {
		return corb_send(w);
	}
	return ics_cmd(w);
}

static uint32_t getp(uint8_t node, uint8_t param)
{
	return corb_cmd(node, VERB_GET_PARAM, param);
}

static uint32_t getv(uint8_t node, uint32_t verb, uint32_t payload)
{
	return corb_cmd(node, verb, payload);
}

static void setv(uint8_t node, uint32_t verb, uint32_t payload)
{
	(void)corb_cmd(node, verb, payload);
}

/** Widget type field from GET_PARAMETER widget caps. */
static unsigned widget_type(uint32_t wcap)
{
	return (wcap >> WCAP_TYPE_SHIFT) & WCAP_TYPE_MASK;
}

/**
 * Unmute an amp at 0 dB when the widget actually has one.
 *
 * Do not poke SET_AMP on widgets without an amp. QEMU's hda-output pin
 * has no amp but defaults stindex to 0, so a "harmless" SET_AMP there
 * overwrites the DAC gain with 0 and the analog path goes silent.
 */
static void unmute(uint8_t node, int output)
{
	uint32_t wcap = getp(node, PARAM_WIDGET_CAPS);
	uint32_t need = output ? WCAP_OUT_AMP : WCAP_IN_AMP;
	if (wcap == 0xFFFFFFFFu || (wcap & need) == 0) {
		return;
	}
	uint32_t cap = getp(node, output ? PARAM_AMP_OUT_CAP : PARAM_AMP_IN_CAP);
	uint32_t offset = (cap == 0xFFFFFFFFu) ? 0 : (cap & 0x7Fu);
	uint32_t payload = (output ? 0xB000u : 0x7000u) | offset;
	setv(node, VERB_SET_AMP, payload);
}

/** Connection-list entry `idx` of `node`. */
static uint8_t conn_at(uint8_t node, unsigned idx, uint32_t lenp)
{
	if (lenp & 0x80u) {
		uint32_t list = getv(node, VERB_GET_CONN_LIST, idx);
		return (uint8_t)(list & 0xFFu);
	}
	uint32_t list = getv(node, VERB_GET_CONN_LIST, idx / 4u);
	if (list == 0xFFFFFFFFu) {
		return 0;
	}
	return (uint8_t)((list >> ((idx % 4u) * 8u)) & 0xFFu);
}

/**
 * Walk pin → mixer/selector → DAC a few hops.
 * Returns the DAC nid, or 0.
 */
static uint8_t find_dac(uint8_t start, uint8_t afg, uint8_t nnodes)
{
	uint8_t node = start;
	for (unsigned hop = 0; hop < 8; hop++) {
		uint32_t wcap = getp(node, PARAM_WIDGET_CAPS);
		if (wcap == 0xFFFFFFFFu) {
			return 0;
		}
		unsigned t = widget_type(wcap);
		if (t == WIDGET_DAC) {
			return node;
		}
		if (wcap & WCAP_OUT_AMP) {
			unmute(node, 1);
		}
		if (wcap & WCAP_IN_AMP) {
			unmute(node, 0);
		}
		uint32_t lenp = getp(node, PARAM_CONN_LIST_LEN);
		unsigned nconn = (lenp == 0xFFFFFFFFu) ? 0 : (lenp & 0x7Fu);
		if (nconn == 0) {
			return 0;
		}
		uint8_t dac = 0;
		uint8_t next = 0;
		for (unsigned i = 0; i < nconn && i < 16; i++) {
			uint8_t c = conn_at(node, i, lenp);
			if (c == 0 || c == node || c < afg || c >= afg + nnodes) {
				continue;
			}
			uint32_t ccaps = getp(c, PARAM_WIDGET_CAPS);
			if (ccaps != 0xFFFFFFFFu && widget_type(ccaps) == WIDGET_DAC) {
				dac = c;
				setv(node, 0x701, i);	/* SET_CONNECT_SEL */
				break;
			}
			if (next == 0) {
				next = c;
			}
		}
		if (dac) {
			return dac;
		}
		if (next == 0) {
			return 0;
		}
		node = next;
	}
	return 0;
}

/**
 * Score a pin for analog playback. The ASRock green "Front Speaker"
 * jack is ALC662 nid 0x14 (line out).
 */
static int pin_score(uint8_t node, uint32_t cfg, uint32_t vid)
{
	unsigned conn = (cfg >> 30) & 3u;
	unsigned dev = (cfg >> 20) & 0xFu;
	unsigned color = (cfg >> 12) & 0xFu;
	if (conn == 1 && !(vid == REALTEK_ALC662 && node == 0x14)) {
		return -1;
	}
	int s = 0;
	if (vid == REALTEK_ALC662 && node == 0x14) {
		s += 100;
	}
	if (dev == PIN_DEV_LINE_OUT) {
		s += 50;
	} else if (dev == PIN_DEV_HP) {
		s += 40;
	} else if (dev == PIN_DEV_SPEAKER) {
		s += 30;
	} else {
		return -1;
	}
	if (color == 4) {
		s += 10;	/* green */
	}
	if (conn == 0 || conn == 2) {
		s += 20;
	}
	return s;
}

/** Program a pin as an analog output and enable EAPD if present. */
static void enable_pin(uint8_t node, int headphone)
{
	uint32_t pincap = getp(node, PARAM_PIN_CAPS);
	setv(node, VERB_SET_PIN_CTRL, headphone ? 0xC0u : 0x40u);
	if (pincap != 0xFFFFFFFFu && (pincap & (1u << 16))) {
		setv(node, VERB_SET_EAPD, 0x02);
	}
	uint32_t wcap = getp(node, PARAM_WIDGET_CAPS);
	if (wcap != 0xFFFFFFFFu && (wcap & WCAP_OUT_AMP)) {
		unmute(node, 1);
	}
	if (wcap != 0xFFFFFFFFu && (wcap & WCAP_IN_AMP)) {
		unmute(node, 0);
	}
}

/** Pick the best analog output pin and its DAC on this AFG. */
static int bind_path(uint8_t afg)
{
	uint32_t sub = getp(afg, PARAM_SUB_NODE);
	if (sub == 0xFFFFFFFFu) {
		return 0;
	}
	uint8_t start = (uint8_t)((sub >> 16) & 0xFFu);
	uint8_t nnodes = (uint8_t)(sub & 0xFFu);
	if (nnodes == 0) {
		return 0;
	}
	setv(afg, VERB_SET_POWER, 0x00);
	hda_delay(200);

	int best = -1;
	uint8_t best_pin = 0;
	uint8_t best_dac = 0;
	for (unsigned i = 0; i < nnodes; i++) {
		uint8_t node = (uint8_t)(start + i);
		uint32_t wcap = getp(node, PARAM_WIDGET_CAPS);
		if (wcap == 0xFFFFFFFFu || widget_type(wcap) != WIDGET_PIN) {
			continue;
		}
		uint32_t cfg = getv(node, VERB_GET_CONFIG, 0);
		if (cfg == 0xFFFFFFFFu) {
			continue;
		}
		int s = pin_score(node, cfg, codec_vid);
		if (s < 0) {
			continue;
		}
		uint8_t dac = find_dac(node, start, nnodes);
		if (dac == 0) {
			continue;
		}
		if (s > best) {
			best = s;
			best_pin = node;
			best_dac = dac;
		}
	}
	if (best < 0) {
		return 0;
	}
	nid_pin = best_pin;
	nid_dac = best_dac;
	nid_afg = afg;
	unsigned dev = (getv(best_pin, VERB_GET_CONFIG, 0) >> 20) & 0xFu;
	enable_pin(best_pin, dev == PIN_DEV_HP);
	setv(best_dac, VERB_SET_POWER, 0x00);
	unmute(best_dac, 1);
	return 1;
}

/** Find an AFG on `codec` and bind analog out. */
static int bind_codec(uint8_t codec)
{
	cad = codec;
	codec_vid = getp(0, PARAM_VENDOR_ID);
	if (codec_vid == 0xFFFFFFFFu || codec_vid == 0) {
		return 0;
	}
	uint32_t sub = getp(0, PARAM_SUB_NODE);
	if (sub == 0xFFFFFFFFu) {
		return 0;
	}
	uint8_t start = (uint8_t)((sub >> 16) & 0xFFu);
	uint8_t n = (uint8_t)(sub & 0xFFu);
	for (unsigned i = 0; i < n; i++) {
		uint8_t node = (uint8_t)(start + i);
		uint32_t fn = getp(node, PARAM_FNG_TYPE);
		if ((fn & 0xFFu) == 0x01u) {
			if (bind_path(node)) {
				return 1;
			}
		}
	}
	return 0;
}

/** Reset CORB/RIRB rings. AMD SB710 can ignore the CORBRP reset bit. */
static int corb_start(void)
{
	mmw8(HDA_CORBCTL, 0);
	mmw8(HDA_RIRBCTL, 0);
	hda_msleep(1);

	mmw32(HDA_CORBLBASE, corb_phys);
	mmw32(HDA_CORBUBASE, 0);
	mmw32(HDA_RIRBLBASE, rirb_phys);
	mmw32(HDA_RIRBUBASE, 0);

	/* Size capability is bits 6:4 (256 / 16 / 2). Program bits 1:0. */
	uint8_t csz = mmr8(HDA_CORBSIZE);
	if (csz & 0x40) {
		mmw8(HDA_CORBSIZE, (uint8_t)((csz & ~0x03) | 0x02));
		corb_mask = 0xFF;
	} else if (csz & 0x20) {
		mmw8(HDA_CORBSIZE, (uint8_t)((csz & ~0x03) | 0x01));
		corb_mask = 0x0F;
	} else {
		mmw8(HDA_CORBSIZE, (uint8_t)(csz & ~0x03));
		corb_mask = 0x01;
	}
	uint8_t rsz = mmr8(HDA_RIRBSIZE);
	if (rsz & 0x40) {
		mmw8(HDA_RIRBSIZE, (uint8_t)((rsz & ~0x03) | 0x02));
	} else if (rsz & 0x20) {
		mmw8(HDA_RIRBSIZE, (uint8_t)((rsz & ~0x03) | 0x01));
	} else {
		mmw8(HDA_RIRBSIZE, (uint8_t)(rsz & ~0x03));
	}

	mmw16(HDA_CORBRP, 0x8000);
	uint64_t deadline = pit_ticks() + 20;
	while ((mmr16(HDA_CORBRP) & 0x8000) == 0 && pit_ticks() < deadline) {
		__asm__ volatile ("pause");
	}
	mmw16(HDA_CORBRP, 0);
	deadline = pit_ticks() + 20;
	while ((mmr16(HDA_CORBRP) & 0x8000) != 0 && pit_ticks() < deadline) {
		__asm__ volatile ("pause");
	}
	mmw16(HDA_CORBWP, 0);
	mmw16(HDA_RIRBWP, 0x8000);
	mmw16(HDA_RINTCNT, 1);
	rirb_rp = 0;

	mmw8(HDA_CORBCTL, CORBCTL_RUN);
	mmw8(HDA_RIRBCTL, RIRBCTL_DMA | RIRBCTL_OIE);
	hda_msleep(1);
	return (mmr8(HDA_CORBCTL) & CORBCTL_RUN) != 0;
}

/** Controller reset, snoop, and codec wake. */
static int controller_reset(void)
{
	mmw32(HDA_INTCTL, 0);
	mmw16(HDA_WAKEEN, 0);
	mmw32(HDA_GCTL, 0);
	uint64_t deadline = pit_ticks() + 50;
	while ((mmr32(HDA_GCTL) & GCTL_CRST) != 0 && pit_ticks() < deadline) {
		__asm__ volatile ("pause");
	}
	mmw32(HDA_GCTL, GCTL_CRST);
	deadline = pit_ticks() + 50;
	while ((mmr32(HDA_GCTL) & GCTL_CRST) == 0 && pit_ticks() < deadline) {
		__asm__ volatile ("pause");
	}
	if ((mmr32(HDA_GCTL) & GCTL_CRST) == 0) {
		return 0;
	}
	/*
	 * Spec minimum after CRST is 521 us. Realtek on SB710 is slower;
	 * the first ICS try on this board returned 0xffffffff.
	 */
	hda_msleep(50);
	mmw16(HDA_WAKEEN, 0x7FFF);
	deadline = pit_ticks() + 100;
	while (mmr16(HDA_STATESTS) == 0 && pit_ticks() < deadline) {
		__asm__ volatile ("pause");
	}
	return 1;
}

/** Walk STATESTS (then the first four slots) and bind analog out. */
static int try_bind(void)
{
	uint16_t statests = mmr16(HDA_STATESTS);
	for (uint8_t c = 0; c < 15; c++) {
		if (statests & (1u << c)) {
			if (bind_codec(c)) {
				return 1;
			}
		}
	}
	for (uint8_t c = 0; c < 4; c++) {
		if (bind_codec(c)) {
			return 1;
		}
	}
	return 0;
}

/** True if this PCI function is an HDA controller. */
static int hda_match(const struct pci_device *d)
{
	if (d->vendor == AMD_SB710_HDA_VEN && d->device == AMD_SB710_HDA_DEV) {
		return 1;
	}
	return d->class_code == 0x04 && d->subclass == 0x03;
}

bool hda_init(void)
{
	present = 0;
	running = 0;
	mmio = NULL;
	unsigned count = pci_device_count();
	const struct pci_device *dev = NULL;
	uint32_t found = UINT32_MAX;

	/* Prefer the AMD SB710 function this board actually has. */
	for (unsigned i = 0; i < count; i++) {
		const struct pci_device *d = pci_device_at(i);
		if (d->vendor == AMD_SB710_HDA_VEN && d->device == AMD_SB710_HDA_DEV) {
			dev = d;
			found = i;
			break;
		}
	}
	if (dev == NULL) {
		for (unsigned i = 0; i < count; i++) {
			const struct pci_device *d = pci_device_at(i);
			if (hda_match(d)) {
				dev = d;
				found = i;
				break;
			}
		}
	}
	if (dev == NULL) {
		tty_printf("hda: no PCI HDA function (%u audio pci)\n", count);
		return false;
	}

	uint64_t bar = pci_mmio_bar(dev, 0);
	if (bar == 0) {
		tty_printf("hda: no MMIO BAR on %x:%x\n",
			(unsigned)dev->vendor, (unsigned)dev->device);
		return false;
	}
	pci_enable_mem_bm(dev);
	ati_enable_snoop(dev);
	if (!phys_map_mmio(bar, 0x4000)) {
		tty_printf("hda: MMIO map failed bar 0x%lx\n", (unsigned long)bar);
		return false;
	}
	mmio = phys_to_virt(bar);

	if (!controller_reset()) {
		tty_printf("hda: controller reset failed\n");
		return false;
	}

	uint16_t gcap = mmr16(HDA_GCAP);
	unsigned iss = (gcap >> 8) & 0xFu;
	sd_off = 0x80u + iss * 0x20u;

	corb = phys_alloc(HDA_CORB_ENTRIES * 4u, &corb_phys);
	rirb = phys_alloc(HDA_CORB_ENTRIES * 8u, &rirb_phys);
	bdl = phys_alloc(sizeof(struct hda_bdl) * HDA_PERIODS, &bdl_phys);
	pcm = phys_alloc(HDA_PERIODS * 256u * 4u, &pcm_phys);
	if (corb == NULL || rirb == NULL || bdl == NULL || pcm == NULL) {
		tty_printf("hda: DMA alloc failed\n");
		return false;
	}

	use_corb = corb_start();
	int bound = try_bind();
	if (!bound && use_corb) {
		/* QEMU ICH9 sometimes ignores CORB; ICS still works there. */
		mmw8(HDA_CORBCTL, 0);
		mmw8(HDA_RIRBCTL, 0);
		use_corb = 0;
		bound = try_bind();
	}
	if (!bound) {
		tty_printf("hda: no output path (vid 0x%x st 0x%x)\n",
			codec_vid, (unsigned)mmr16(HDA_STATESTS));
		return false;
	}

	if (codec_vid == REALTEK_ALC662) {
		ksnprintf(name, sizeof(name), "SB710 HDA ALC662 pin %u dac %u",
			(unsigned)nid_pin, (unsigned)nid_dac);
	} else {
		ksnprintf(name, sizeof(name), "HDA %x pin %u dac %u",
			codec_vid, (unsigned)nid_pin, (unsigned)nid_dac);
	}
	bound_bus = dev->bus;
	bound_slot = dev->slot;
	bound_func = dev->func;
	bound_index = found;
	hw_rate = 48000;
	present = 1;
	return true;
}

bool hda_present(void)
{
	return present != 0;
}

uint32_t hda_pci_index(void)
{
	return present ? bound_index : UINT32_MAX;
}

bool hda_alive(void)
{
	if (!present) {
		return false;
	}
	return pci_read16(bound_bus, bound_slot, bound_func, 0x00) != 0xFFFF;
}

const char *hda_name(void)
{
	return present ? name : "none";
}

uint32_t hda_hw_rate(void)
{
	return hw_rate;
}

/**
 * Push CPU-written PCM out of cache so SB710 DMA sees it.
 * ATI snoop covers most of this; clflush is the belt for choppy underruns.
 */
static void dma_flush(void *p, size_t n)
{
	uint8_t *b = (uint8_t *)p;
	for (size_t i = 0; i < n; i += 64u) {
		__asm__ volatile ("clflush (%0)" : : "r"(b + i) : "memory");
	}
	__asm__ volatile ("mfence" ::: "memory");
}

/** Reset one stream descriptor and wait for SRST to clear. */
static int sd_reset(void)
{
	mmw8(sd_off + SD_CTL, 0);
	hda_delay(20);
	mmw8(sd_off + SD_CTL, SD_CTL_SRST);
	for (unsigned i = 0; i < 4000; i++) {
		if (mmr8(sd_off + SD_CTL) & SD_CTL_SRST) {
			break;
		}
		hda_delay(1);
	}
	mmw8(sd_off + SD_CTL, 0);
	for (unsigned i = 0; i < 4000; i++) {
		if ((mmr8(sd_off + SD_CTL) & SD_CTL_SRST) == 0) {
			return 1;
		}
		hda_delay(1);
	}
	return 0;
}

bool hda_start(uint32_t frames, void (*fill)(int16_t *dst, uint32_t frames))
{
	if (!present || fill == NULL || bdl == NULL || pcm == NULL) {
		return false;
	}
	if (frames < 16) {
		frames = 16;
	}
	if (frames > 256) {
		frames = 256;
	}
	period_frames = frames;
	period_bytes = frames * 4u;
	underruns = 0;
	if (!sd_reset()) {
		return false;
	}

	for (unsigned i = 0; i < HDA_PERIODS; i++) {
		bdl[i].addr = (uint64_t)pcm_phys + (uint64_t)i * period_bytes;
		bdl[i].len = period_bytes;
		bdl[i].ioc = 0;
		fill((int16_t *)(pcm + i * period_bytes), frames);
		dma_flush(pcm + i * period_bytes, period_bytes);
	}
	next_fill = 0;

	mmw32(sd_off + SD_CBL, period_bytes * HDA_PERIODS);
	mmw16(sd_off + SD_LVI, HDA_PERIODS - 1);
	mmw16(sd_off + SD_FMT, HDA_FMT_48K_S16_2CH);
	mmw32(sd_off + SD_BDPL, bdl_phys);
	mmw32(sd_off + SD_BDPU, 0);
	mmw8(sd_off + SD_STS, 0x1C);

	setv(nid_dac, VERB_SET_CONV_FMT, HDA_FMT_48K_S16_2CH);
	setv(nid_dac, VERB_SET_STREAM_CHAN, (HDA_STREAM_TAG << 4));
	unmute(nid_dac, 1);
	unmute(nid_pin, 1);

	/*
	 * Stream tag is SDCTL bits 23:20. Write tag+RUN as one dword so a
	 * byte store cannot drop the tag (QEMU and SB710 both treat CTL as
	 * a 24-bit register overlapping STS).
	 */
	mmw32(sd_off + SD_CTL, (HDA_STREAM_TAG << 20) | SD_CTL_RUN);
	running = 1;
	return true;
}

/** Re-arm DMA if the controller dropped RUN after an underrun. */
static void sd_kick(void)
{
	mmw32(sd_off + SD_CBL, period_bytes * HDA_PERIODS);
	mmw16(sd_off + SD_LVI, HDA_PERIODS - 1);
	mmw16(sd_off + SD_FMT, HDA_FMT_48K_S16_2CH);
	mmw32(sd_off + SD_BDPL, bdl_phys);
	mmw32(sd_off + SD_BDPU, 0);
	mmw8(sd_off + SD_STS, 0x1C);
	mmw32(sd_off + SD_CTL, (HDA_STREAM_TAG << 20) | SD_CTL_RUN);
}

void hda_stop(void)
{
	if (!present) {
		return;
	}
	mmw8(sd_off + SD_CTL, 0);
	hda_delay(20);
	mmw8(sd_off + SD_CTL, SD_CTL_SRST);
	hda_delay(20);
	mmw8(sd_off + SD_CTL, 0);
	if (nid_dac) {
		setv(nid_dac, VERB_SET_STREAM_CHAN, 0);
	}
	running = 0;
}

uint32_t hda_underruns(void)
{
	return underruns;
}

unsigned hda_service(void (*fill)(int16_t *dst, uint32_t frames))
{
	if (!present || !running || fill == NULL || period_bytes == 0) {
		return 0;
	}
	uint8_t sts = mmr8(sd_off + SD_STS);
	if (sts & (SD_STS_FIFOE | SD_STS_DESE)) {
		underruns++;
	}
	mmw8(sd_off + SD_STS, sts);

	uint32_t lpib = mmr32(sd_off + SD_LPIB);
	uint32_t hw = (lpib / period_bytes) % HDA_PERIODS;
	unsigned filled = 0;
	while (next_fill != hw && filled < HDA_PERIODS) {
		int16_t *dst = (int16_t *)(pcm + next_fill * period_bytes);
		fill(dst, period_frames);
		dma_flush(dst, period_bytes);
		next_fill = (uint8_t)((next_fill + 1) % HDA_PERIODS);
		filled++;
	}
	if ((mmr8(sd_off + SD_CTL) & SD_CTL_RUN) == 0) {
		underruns++;
		sd_kick();
	}
	return filled;
}

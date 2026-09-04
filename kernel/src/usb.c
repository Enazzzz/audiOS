#include "usb.h"
#include "klib.h"
#include "pci.h"
#include "phys.h"
#include "pit.h"
#include "tty.h"

#include <stddef.h>

#define EHCI_CLASS	0x0C
#define EHCI_SUB	0x03
#define EHCI_PI		0x20

#define USB_TIMEOUT	500u

/* Operational registers */
#define USBCMD		0x00
#define USBSTS		0x04
#define USBINTR		0x08
#define FRINDEX		0x0C
#define CTRLDSSEG	0x10
#define PERIODICLIST	0x14
#define ASYNCLIST	0x18
#define CONFIGFLAG	0x40
#define PORTSC		0x44

#define CMD_RS		0x00000001u
#define CMD_HCRESET	0x00000002u
#define CMD_ASE		0x00000020u
#define STS_HCHALTED	0x00001000u
#define PORT_CCS	0x00000001u
#define PORT_PED	0x00000004u
#define PORT_PR		0x00000100u
#define PORT_PP		0x00001000u
#define PORT_OWNER	0x00002000u

#define QTD_ACT		0x80u
#define QTD_HALT	0x40u
#define PID_OUT		0
#define PID_IN		1
#define PID_SETUP	2

#define CBW_SIG		0x43425355u
#define CSW_SIG		0x53425355u

struct ehci_qtd {
	uint32_t next;
	uint32_t alt;
	uint32_t token;
	uint32_t buf[5];
	uint32_t buf_hi[5];
} __attribute__((packed, aligned(32)));

struct ehci_qh {
	uint32_t hlp;
	uint32_t epchar;
	uint32_t epcap;
	uint32_t current;
	uint32_t next;
	uint32_t alt;
	uint32_t token;
	uint32_t buf[5];
	uint32_t buf_hi[5];
} __attribute__((packed, aligned(32)));

struct setup_pkt {
	uint8_t type;
	uint8_t req;
	uint16_t value;
	uint16_t index;
	uint16_t length;
} __attribute__((packed));

struct cbw {
	uint32_t sig;
	uint32_t tag;
	uint32_t data_len;
	uint8_t flags;
	uint8_t lun;
	uint8_t cb_len;
	uint8_t cb[16];
} __attribute__((packed));

struct csw {
	uint32_t sig;
	uint32_t tag;
	uint32_t residue;
	uint8_t status;
} __attribute__((packed));

static volatile uint8_t *cap;
static volatile uint8_t *op;
static unsigned nports;
static struct ehci_qh *qh_async;
static struct ehci_qh *qh_ep;
static struct ehci_qtd *qtds;
static uint32_t qh_async_phys;
static uint32_t qh_ep_phys;
static uint32_t qtd_phys;
static uint8_t *bounce;
static uint32_t bounce_phys;
static void (*idle_fn)(void);
static uint8_t dev_addr;
static uint8_t ep_in;
static uint8_t ep_out;
static uint16_t maxpkt;
static uint32_t scsi_tag;
static uint64_t msc_sectors;
static char msc_name[40];
static int msc_ready;
static unsigned tog_in;
static unsigned tog_out;

static uint32_t cap32(uint32_t off)
{
	return *(volatile uint32_t *)(cap + off);
}

static uint8_t cap8(uint32_t off)
{
	return *(volatile uint8_t *)(cap + off);
}

static uint32_t opr(uint32_t off)
{
	return *(volatile uint32_t *)(op + off);
}

static void opw(uint32_t off, uint32_t v)
{
	*(volatile uint32_t *)(op + off) = v;
}

/** Spin until `ms` elapse, pumping audio so HDA does not underrun. */
static void usb_wait_ms(uint32_t ms)
{
	uint64_t start = pit_ticks();
	while (pit_ticks() - start < (uint64_t)ms) {
		if (idle_fn) {
			idle_fn();
		}
		__asm__ volatile ("pause");
	}
}

/** EHCI BIOS handoff via the USBLEGSUP capability. */
static void ehci_handoff(const struct pci_device *dev)
{
	uint32_t hcc = cap32(0x08);
	uint8_t eecp = (uint8_t)((hcc >> 8) & 0xFF);
	if (eecp < 0x40) {
		return;
	}
	uint32_t leg = pci_read32(dev->bus, dev->slot, dev->func, eecp);
	pci_write32(dev->bus, dev->slot, dev->func, eecp, leg | (1u << 24));
	uint64_t deadline = pit_ticks() + 200;
	while (pit_ticks() < deadline) {
		leg = pci_read32(dev->bus, dev->slot, dev->func, eecp);
		if ((leg & (1u << 16)) == 0) {
			break;
		}
		usb_wait_ms(1);
	}
}

/** Reset the host controller and start the asynchronous schedule. */
static int ehci_start(void)
{
	opw(USBINTR, 0);
	opw(USBCMD, opr(USBCMD) | CMD_HCRESET);
	uint64_t deadline = pit_ticks() + 100;
	while ((opr(USBCMD) & CMD_HCRESET) && pit_ticks() < deadline) {
		__asm__ volatile ("pause");
	}
	if (opr(USBCMD) & CMD_HCRESET) {
		return 0;
	}
	opw(CTRLDSSEG, 0);
	opw(USBINTR, 0);
	opw(USBSTS, 0x3F);
	opw(PERIODICLIST, 0);

	memset(qh_async, 0, sizeof(*qh_async));
	qh_async->hlp = qh_async_phys | 0x02;
	qh_async->epchar = (1u << 15) | (1u << 14) | (2u << 12); /* H, DTC, HS */
	qh_async->token = QTD_HALT;
	qh_async->next = 1;
	qh_async->alt = 1;
	__asm__ volatile ("mfence" ::: "memory");
	opw(ASYNCLIST, qh_async_phys);
	opw(USBCMD, (8u << 16) | CMD_ASE | CMD_RS);
	usb_wait_ms(2);
	opw(CONFIGFLAG, 1);
	usb_wait_ms(10);
	return (opr(USBSTS) & STS_HCHALTED) == 0;
}

/** Program `qh_ep` for `addr` / `ep` (0 = control). */
static void qh_bind(uint8_t addr, uint8_t ep, int in, uint16_t pkt)
{
	uint32_t epchar = (uint32_t)pkt << 16;
	epchar |= (1u << 14);		/* DTC */
	epchar |= (2u << 12);		/* HS */
	epchar |= ((uint32_t)ep << 8);
	epchar |= addr;
	(void)in;
	qh_ep->hlp = qh_async_phys | 0x02;
	qh_ep->epchar = epchar;
	qh_ep->epcap = 0x40000000u;	/* MULT = 1 */
	qh_ep->current = 0;
	qh_ep->next = 1;
	qh_ep->alt = 1;
	qh_ep->token = 0;
	memset(qh_ep->buf, 0, sizeof(qh_ep->buf));
	memset(qh_ep->buf_hi, 0, sizeof(qh_ep->buf_hi));
	qh_async->hlp = qh_ep_phys | 0x02;
	__asm__ volatile ("mfence" ::: "memory");
}

/** Run one qTD and wait for it to complete. Returns 0 on success. */
static int qtd_run(struct ehci_qtd *td)
{
	qh_ep->next = qtd_phys + (uint32_t)((uint8_t *)td - (uint8_t *)qtds);
	qh_ep->alt = 1;
	qh_ep->token = 0;
	qh_ep->current = 0;
	__asm__ volatile ("mfence" ::: "memory");
	uint64_t deadline = pit_ticks() + USB_TIMEOUT;
	while (pit_ticks() < deadline) {
		uint32_t tok = td->token;
		if ((tok & QTD_ACT) == 0) {
			if (tok & QTD_HALT) {
				return -1;
			}
			return 0;
		}
		if (idle_fn) {
			idle_fn();
		}
		__asm__ volatile ("pause");
	}
	return -1;
}

/** Fill a qTD for `pid` against `phys`/`len`. Data toggle in `dt`. */
static void qtd_fill(struct ehci_qtd *td, uint32_t phys, uint32_t len, unsigned pid, unsigned dt)
{
	memset(td, 0, sizeof(*td));
	td->next = 1;
	td->alt = 1;
	td->token = (dt << 31) | (len << 16) | (3u << 10) | (pid << 8) | QTD_ACT;
	td->buf[0] = phys;
	td->buf[1] = (phys + 0x1000u) & ~0xFFFu;
	td->buf[2] = (phys + 0x2000u) & ~0xFFFu;
	td->buf[3] = (phys + 0x3000u) & ~0xFFFu;
	td->buf[4] = (phys + 0x4000u) & ~0xFFFu;
	memset(td->buf_hi, 0, sizeof(td->buf_hi));
}

/** Control transfer on endpoint 0. */
static int ctrl(uint8_t addr, uint8_t type, uint8_t req, uint16_t value,
	uint16_t index, void *data, uint16_t len)
{
	struct setup_pkt *setup = (struct setup_pkt *)bounce;
	memset(setup, 0, sizeof(*setup));
	setup->type = type;
	setup->req = req;
	setup->value = value;
	setup->index = index;
	setup->length = len;
	qh_bind(addr, 0, 0, 64);
	qtd_fill(&qtds[0], bounce_phys, 8, PID_SETUP, 0);
	if (qtd_run(&qtds[0]) != 0) {
		return -1;
	}
	unsigned dt = 1;
	if (len != 0 && data != NULL) {
		unsigned pid = (type & 0x80) ? PID_IN : PID_OUT;
		if (pid == PID_OUT) {
			memcpy(bounce + 64, data, len);
		}
		qtd_fill(&qtds[1], bounce_phys + 64, len, pid, dt);
		if (qtd_run(&qtds[1]) != 0) {
			return -1;
		}
		if (pid == PID_IN) {
			memcpy(data, bounce + 64, len);
		}
		dt ^= 1;
	}
	unsigned stpid = (type & 0x80) ? PID_OUT : PID_IN;
	if (len == 0) {
		stpid = PID_IN;
	}
	qtd_fill(&qtds[2], bounce_phys + 64, 0, stpid, 1);
	return qtd_run(&qtds[2]);
}

/** Bulk transfer on the MSC endpoints. */
static int bulk(int in, void *data, uint32_t len)
{
	uint8_t ep = in ? ep_in : ep_out;
	qh_bind(dev_addr, ep, in, maxpkt);
	if (!in && data != NULL && len > 0) {
		memcpy(bounce, data, len);
	}
	unsigned *tog = in ? &tog_in : &tog_out;
	qtd_fill(&qtds[0], bounce_phys, len, in ? PID_IN : PID_OUT, *tog);
	if (qtd_run(&qtds[0]) != 0) {
		return -1;
	}
	*tog ^= 1u;
	if (in && data != NULL && len > 0) {
		for (uint32_t i = 0; i < len; i += 64u) {
			__asm__ volatile ("clflush (%0)" : : "r"(bounce + i) : "memory");
		}
		__asm__ volatile ("mfence" ::: "memory");
		memcpy(data, bounce, len);
	}
	return 0;
}

/** SCSI command over BOT. `data` may be NULL. */
static int scsi(uint8_t *cdb, uint8_t cdblen, int in, void *data, uint32_t len)
{
	struct cbw cb;
	memset(&cb, 0, sizeof(cb));
	cb.sig = CBW_SIG;
	cb.tag = ++scsi_tag;
	cb.data_len = len;
	cb.flags = (in && len > 0) ? 0x80u : 0;
	cb.cb_len = cdblen;
	memcpy(cb.cb, cdb, cdblen);
	if (bulk(0, &cb, 31) != 0) {
		return -1;
	}
	if (len > 0) {
		if (bulk(in, data, len) != 0) {
			return -1;
		}
	}
	struct csw st;
	memset(&st, 0, sizeof(st));
	if (bulk(1, &st, 13) != 0) {
		return -1;
	}
	if (st.sig != CSW_SIG || st.status != 0) {
		return -1;
	}
	return 0;
}

static int msc_read_lba(uint64_t lba, uint32_t count, void *buf)
{
	uint8_t *dst = buf;
	while (count > 0) {
		uint32_t n = count;
		if (n > 32) {
			n = 32;
		}
		uint8_t cdb[16];
		memset(cdb, 0, sizeof(cdb));
		cdb[0] = 0x28;
		cdb[2] = (uint8_t)(lba >> 24);
		cdb[3] = (uint8_t)(lba >> 16);
		cdb[4] = (uint8_t)(lba >> 8);
		cdb[5] = (uint8_t)lba;
		cdb[7] = (uint8_t)(n >> 8);
		cdb[8] = (uint8_t)n;
		if (scsi(cdb, 10, 1, dst, n * 512u) != 0) {
			return -1;
		}
		dst += n * 512u;
		lba += n;
		count -= n;
	}
	return 0;
}

static int msc_write_lba(uint64_t lba, uint32_t count, const void *buf)
{
	const uint8_t *src = buf;
	while (count > 0) {
		uint32_t n = count;
		if (n > 32) {
			n = 32;
		}
		uint8_t cdb[16];
		memset(cdb, 0, sizeof(cdb));
		cdb[0] = 0x2A;
		cdb[2] = (uint8_t)(lba >> 24);
		cdb[3] = (uint8_t)(lba >> 16);
		cdb[4] = (uint8_t)(lba >> 8);
		cdb[5] = (uint8_t)lba;
		cdb[7] = (uint8_t)(n >> 8);
		cdb[8] = (uint8_t)n;
		if (scsi(cdb, 10, 0, (void *)src, n * 512u) != 0) {
			return -1;
		}
		src += n * 512u;
		lba += n;
		count -= n;
	}
	return 0;
}

/** Parse a config descriptor for BOT MSC bulk endpoints. */
static int parse_msc(const uint8_t *cfg, uint16_t total)
{
	ep_in = 0;
	ep_out = 0;
	maxpkt = 512;
	size_t off = 0;
	int want = 0;
	while (off + 2 <= total) {
		uint8_t len = cfg[off];
		uint8_t typ = cfg[off + 1];
		if (len < 2 || off + len > total) {
			break;
		}
		if (typ == 4 && len >= 9) {
			want = (cfg[off + 5] == 8 && cfg[off + 6] == 6 && cfg[off + 7] == 0x50);
		} else if (want && typ == 5 && len >= 7) {
			uint8_t addr = cfg[off + 2];
			uint8_t attr = cfg[off + 3];
			if ((attr & 3) == 2) {
				if (addr & 0x80) {
					ep_in = addr & 0x0F;
				} else {
					ep_out = addr & 0x0F;
				}
				maxpkt = (uint16_t)(cfg[off + 4] | (cfg[off + 5] << 8));
				if (maxpkt == 0) {
					maxpkt = 512;
				}
			}
		}
		off += len;
	}
	return ep_in && ep_out;
}

/** SET_ADDRESS then read the config and bring up MSC on `addr`. */
static int try_msc(uint8_t addr)
{
	uint8_t dd[18];
	memset(dd, 0, sizeof(dd));
	if (ctrl(0, 0x80, 6, 0x0100, 0, dd, 18) != 0) {
		return 0;
	}
	if (ctrl(0, 0x00, 5, addr, 0, NULL, 0) != 0) {
		return 0;
	}
	dev_addr = addr;
	usb_wait_ms(2);
	uint8_t hdr[8];
	memset(hdr, 0, sizeof(hdr));
	if (ctrl(addr, 0x80, 6, 0x0200, 0, hdr, 8) != 0) {
		return 0;
	}
	uint16_t total = (uint16_t)(hdr[2] | (hdr[3] << 8));
	if (total < 9 || total > 512) {
		return 0;
	}
	uint8_t cfg[512];
	memset(cfg, 0, sizeof(cfg));
	if (ctrl(addr, 0x80, 6, 0x0200, 0, cfg, total) != 0) {
		return 0;
	}
	uint8_t cfgval = cfg[5];
	if (ctrl(addr, 0x00, 9, cfgval, 0, NULL, 0) != 0) {
		return 0;
	}
	if (!parse_msc(cfg, total)) {
		return 0;
	}
	tog_in = 0;
	tog_out = 0;
	usb_wait_ms(10);
	uint8_t cdb[16];
	for (unsigned t = 0; t < 5; t++) {
		memset(cdb, 0, sizeof(cdb));
		if (scsi(cdb, 6, 1, NULL, 0) == 0) {
			break;
		}
		usb_wait_ms(50);
	}
	memset(cdb, 0, sizeof(cdb));
	cdb[0] = 0x12;
	cdb[4] = 36;
	uint8_t inq[36];
	memset(inq, 0, sizeof(inq));
	(void)scsi(cdb, 6, 1, inq, 36);
	memset(cdb, 0, sizeof(cdb));
	cdb[0] = 0x25;
	uint8_t cap[8];
	memset(cap, 0, sizeof(cap));
	if (scsi(cdb, 10, 1, cap, 8) != 0) {
		return 0;
	}
	uint32_t last = ((uint32_t)cap[0] << 24) | ((uint32_t)cap[1] << 16)
		| ((uint32_t)cap[2] << 8) | cap[3];
	uint32_t bps = ((uint32_t)cap[4] << 24) | ((uint32_t)cap[5] << 16)
		| ((uint32_t)cap[6] << 8) | cap[7];
	if (bps != 512) {
		return 0;
	}
	msc_sectors = (uint64_t)last + 1ull;
	ksnprintf(msc_name, sizeof(msc_name), "USB MSC %llu sectors",
		(unsigned long long)msc_sectors);
	msc_ready = 1;
	return 1;
}

/** Reset one root port and try to install MSC. */
static int port_probe(unsigned i, uint8_t *next_addr)
{
	uint32_t sc = opr(PORTSC + i * 4);
	if ((sc & PORT_CCS) == 0) {
		return 0;
	}
	opw(PORTSC + i * 4, sc | PORT_PP);
	usb_wait_ms(20);
	sc = opr(PORTSC + i * 4);
	opw(PORTSC + i * 4, (sc & ~PORT_PED) | PORT_PR);
	usb_wait_ms(50);
	sc = opr(PORTSC + i * 4);
	opw(PORTSC + i * 4, sc & ~PORT_PR);
	usb_wait_ms(10);
	sc = opr(PORTSC + i * 4);
	if ((sc & PORT_PED) == 0) {
		/* Full/low-speed (a USB keyboard, etc.): park it so EHCI
		 * does not stall. Keyboard input is PS/2, not USB HID. */
		opw(PORTSC + i * 4, sc | PORT_OWNER);
		return 0;
	}
	uint8_t addr = *next_addr;
	if (try_msc(addr)) {
		return 1;
	}
	(*next_addr)++;
	return 0;
}

bool usb_msc_init(struct blkdev *out, void (*idle)(void))
{
	msc_ready = 0;
	idle_fn = idle;
	qh_async = phys_alloc(sizeof(struct ehci_qh), &qh_async_phys);
	qh_ep = phys_alloc(sizeof(struct ehci_qh), &qh_ep_phys);
	qtds = phys_alloc(sizeof(struct ehci_qtd) * 4, &qtd_phys);
	bounce = phys_alloc(32u * 512u, &bounce_phys);
	if (qh_async == NULL || qh_ep == NULL || qtds == NULL || bounce == NULL) {
		tty_puts("usb: DMA alloc failed\n");
		return false;
	}

	int any = 0;
	for (unsigned nth = 0; nth < 6; nth++) {
		struct pci_device dev;
		if (!pci_find_class(EHCI_CLASS, EHCI_SUB, EHCI_PI, nth, &dev)) {
			break;
		}
		any = 1;
		uint64_t bar = pci_mmio_bar(&dev, 0);
		if (bar == 0) {
			continue;
		}
		pci_enable_mem_bm(&dev);
		if (!phys_map_mmio(bar, 0x1000)) {
			continue;
		}
		cap = phys_to_virt(bar);
		uint8_t caplen = cap8(0);
		if (caplen < 0x10 || caplen > 0x80) {
			continue;
		}
		op = cap + caplen;
		nports = cap32(0x04) & 0x0Fu;
		if (nports == 0) {
			nports = 8;
		}
		ehci_handoff(&dev);
		if (!ehci_start()) {
			continue;
		}
		uint8_t addr = 1;
		int found = 0;
		for (unsigned i = 0; i < nports; i++) {
			if (port_probe(i, &addr)) {
				out->read = msc_read_lba;
				out->write = msc_write_lba;
				out->sectors = msc_sectors;
				ksnprintf(out->name, sizeof(out->name), "%s", msc_name);
				tty_printf("usb: %s\n", msc_name);
				found = 1;
				break;
			}
		}
		if (found) {
			return true;
		}
	}
	if (!any) {
		tty_puts("usb: no EHCI controller\n");
	} else {
		tty_puts("usb: no high-speed mass storage\n");
	}
	return false;
}

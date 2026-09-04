#include "usb.h"
#include "kbd.h"
#include "klib.h"
#include "pci.h"
#include "phys.h"
#include "pit.h"
#include "tty.h"

#include <stddef.h>

#define OHCI_CLASS	0x0C
#define OHCI_SUB	0x03
#define OHCI_PI		0x10

#define HC_REVISION		0x00
#define HC_CONTROL		0x04
#define HC_CMDSTATUS		0x08
#define HC_INTSTATUS		0x0C
#define HC_INTENABLE		0x10
#define HC_INTDISABLE		0x14
#define HC_HCCA			0x18
#define HC_CONTROLHEADED	0x20
#define HC_CONTROLCURRENTED	0x24
#define HC_FMINTERVAL		0x34
#define HC_FMREMAINING		0x38
#define HC_PERIODICSTART	0x40
#define HC_RHDESCA		0x48
#define HC_RHSTATUS		0x50
#define HC_RHPORTSTATUS		0x54

#define CTRL_PLE	(1u << 2)
#define CTRL_CLE	(1u << 4)
#define CTRL_IR		(1u << 8)
#define CTRL_USBOPER	(2u << 6)

#define CMD_HCR		(1u << 0)
#define CMD_CLF		(1u << 1)
#define CMD_OCR		(1u << 3)

#define PORT_CCS	(1u << 0)
#define PORT_PES	(1u << 1)
#define PORT_PRS	(1u << 4)
#define PORT_PPS	(1u << 8)
#define PORT_LSDA	(1u << 9)
#define PORT_CSC	(1u << 16)
#define PORT_PESC	(1u << 17)
#define PORT_PRSC	(1u << 20)

#define ED_SKIP		(1u << 14)
#define ED_LOWSPEED	(1u << 13)
#define ED_IN		(2u << 11)
#define TD_R		(1u << 18)
#define TD_DP_SETUP	0u
#define TD_DP_OUT	(1u << 19)
#define TD_DP_IN	(2u << 19)
#define TD_T_DATA0	(2u << 24)
#define TD_T_DATA1	(3u << 24)
#define TD_DI_NONE	(7u << 21)
#define TD_CC_SHIFT	28

struct ohci_ed {
	volatile uint32_t flags;
	volatile uint32_t tail;
	volatile uint32_t head;
	volatile uint32_t next;
} __attribute__((packed, aligned(16)));

struct ohci_td {
	volatile uint32_t flags;
	volatile uint32_t cbp;
	volatile uint32_t next;
	volatile uint32_t be;
} __attribute__((packed, aligned(16)));

struct ohci_hcca {
	uint32_t inttable[32];
	uint16_t framenumber;
	uint16_t pad1;
	uint32_t donehead;
	uint8_t reserved[120];
} __attribute__((packed, aligned(256)));

static volatile uint8_t *op;
static struct ohci_hcca *hcca;
static uint32_t hcca_phys;
static struct ohci_ed *eds;
static uint32_t eds_phys;
static struct ohci_td *tds;
static uint32_t tds_phys;
static uint8_t *obuf;
static uint32_t obuf_phys;
static void (*idle_fn)(void);

static int hid_ready;
static uint8_t hid_addr;
static uint8_t hid_iface;
static uint8_t hid_ls;
static uint8_t hid_prev[8];
static uint8_t hid_ep;
static uint8_t hid_mps;
static int hid_use_int;

/** Pump audio while waiting on the host controller. */
static void ohci_idle(void)
{
	if (idle_fn) {
		idle_fn();
	}
	__asm__ volatile ("pause");
}

static uint32_t rr(uint32_t off)
{
	return *(volatile uint32_t *)(op + off);
}

static void rw(uint32_t off, uint32_t v)
{
	*(volatile uint32_t *)(op + off) = v;
}

/** Flush DMA descriptors so OHCI sees the CPU writes. */
static void ohci_sync(void)
{
	__asm__ volatile ("mfence" ::: "memory");
}

static uint32_t td_phys(unsigned i)
{
	return tds_phys + (uint32_t)(i * sizeof(struct ohci_td));
}

static uint32_t ed_phys(unsigned i)
{
	return eds_phys + (uint32_t)(i * sizeof(struct ohci_ed));
}

/** Wait until a TD leaves the "not accessed" condition code. */
static int td_wait(struct ohci_td *td, uint32_t ms)
{
	uint64_t deadline = pit_ticks() + ms;
	while (pit_ticks() < deadline) {
		uint32_t cc = td->flags >> TD_CC_SHIFT;
		if (cc != 0xF) {
			return cc == 0 ? 0 : -1;
		}
		ohci_idle();
	}
	return -1;
}

/**
 * Run an EP0 control transfer on `addr`. `data` may be NULL when `len` is 0.
 */
static int ohci_ctrl(uint8_t addr, uint8_t type, uint8_t req, uint16_t value,
	uint16_t index, void *data, uint16_t len, int ls)
{
	struct setup_pkt {
		uint8_t type;
		uint8_t req;
		uint16_t value;
		uint16_t index;
		uint16_t length;
	} __attribute__((packed));

	struct setup_pkt *setup = (struct setup_pkt *)obuf;
	memset(obuf, 0, 64);
	setup->type = type;
	setup->req = req;
	setup->value = value;
	setup->index = index;
	setup->length = len;
	if (len != 0 && data != NULL && (type & 0x80) == 0) {
		memcpy(obuf + 8, data, len);
	}

	unsigned ntd = (len != 0) ? 3 : 2;
	memset(tds, 0, sizeof(struct ohci_td) * 4);
	/* Dummy tail TD (index ntd). */
	tds[ntd].flags = 0;
	tds[ntd].next = 0;

	tds[0].flags = TD_DP_SETUP | TD_T_DATA0 | TD_DI_NONE | (0xFu << TD_CC_SHIFT);
	tds[0].cbp = obuf_phys;
	tds[0].be = obuf_phys + 7;
	tds[0].next = td_phys(1);

	if (len != 0) {
		unsigned dp = (type & 0x80) ? TD_DP_IN : TD_DP_OUT;
		tds[1].flags = dp | TD_T_DATA1 | TD_R | TD_DI_NONE | (0xFu << TD_CC_SHIFT);
		tds[1].cbp = obuf_phys + 8;
		tds[1].be = obuf_phys + 8 + (len ? (len - 1) : 0);
		tds[1].next = td_phys(2);
		unsigned st = (type & 0x80) ? TD_DP_OUT : TD_DP_IN;
		tds[2].flags = st | TD_T_DATA1 | TD_DI_NONE | (0xFu << TD_CC_SHIFT);
		tds[2].cbp = 0;
		tds[2].be = 0;
		tds[2].next = td_phys(3);
	} else {
		tds[1].flags = TD_DP_IN | TD_T_DATA1 | TD_DI_NONE | (0xFu << TD_CC_SHIFT);
		tds[1].cbp = 0;
		tds[1].be = 0;
		tds[1].next = td_phys(2);
	}

	uint32_t flags = addr | (8u << 16);
	if (ls) {
		flags |= ED_LOWSPEED;
	}
	memset(&eds[0], 0, sizeof(eds[0]));
	eds[0].flags = flags | ED_SKIP;
	eds[0].tail = td_phys(ntd);
	eds[0].head = td_phys(0);
	eds[0].next = 0;
	ohci_sync();
	eds[0].flags = flags;

	rw(HC_CONTROLHEADED, ed_phys(0));
	rw(HC_CONTROLCURRENTED, 0);
	rw(HC_CMDSTATUS, rr(HC_CMDSTATUS) | CMD_CLF);
	rw(HC_CONTROL, (rr(HC_CONTROL) & ~(CTRL_IR | (3u << 6))) | CTRL_CLE | CTRL_USBOPER);
	ohci_sync();

	int rc = 0;
	for (unsigned i = 0; i < ntd; i++) {
		if (td_wait(&tds[i], 400) != 0) {
			rc = -1;
			break;
		}
	}
	eds[0].flags |= ED_SKIP;
	ohci_sync();
	if (rc == 0 && len != 0 && data != NULL && (type & 0x80)) {
		memcpy(data, obuf + 8, len);
	}
	return rc;
}

/** HID boot-protocol usage → audiOS key. */
static int hid_usage(uint8_t code, int shift)
{
	static const char unshifted[] = {
		[0x04] = 'a', [0x05] = 'b', [0x06] = 'c', [0x07] = 'd',
		[0x08] = 'e', [0x09] = 'f', [0x0A] = 'g', [0x0B] = 'h',
		[0x0C] = 'i', [0x0D] = 'j', [0x0E] = 'k', [0x0F] = 'l',
		[0x10] = 'm', [0x11] = 'n', [0x12] = 'o', [0x13] = 'p',
		[0x14] = 'q', [0x15] = 'r', [0x16] = 's', [0x17] = 't',
		[0x18] = 'u', [0x19] = 'v', [0x1A] = 'w', [0x1B] = 'x',
		[0x1C] = 'y', [0x1D] = 'z', [0x1E] = '1', [0x1F] = '2',
		[0x20] = '3', [0x21] = '4', [0x22] = '5', [0x23] = '6',
		[0x24] = '7', [0x25] = '8', [0x26] = '9', [0x27] = '0',
		[0x2C] = ' ', [0x2D] = '-', [0x2E] = '=', [0x2F] = '[',
		[0x30] = ']', [0x31] = '\\', [0x33] = ';', [0x34] = '\'',
		[0x35] = '`', [0x36] = ',', [0x37] = '.', [0x38] = '/',
	};
	static const char shifted[] = {
		[0x04] = 'A', [0x05] = 'B', [0x06] = 'C', [0x07] = 'D',
		[0x08] = 'E', [0x09] = 'F', [0x0A] = 'G', [0x0B] = 'H',
		[0x0C] = 'I', [0x0D] = 'J', [0x0E] = 'K', [0x0F] = 'L',
		[0x10] = 'M', [0x11] = 'N', [0x12] = 'O', [0x13] = 'P',
		[0x14] = 'Q', [0x15] = 'R', [0x16] = 'S', [0x17] = 'T',
		[0x18] = 'U', [0x19] = 'V', [0x1A] = 'W', [0x1B] = 'X',
		[0x1C] = 'Y', [0x1D] = 'Z', [0x1E] = '!', [0x1F] = '@',
		[0x20] = '#', [0x21] = '$', [0x22] = '%', [0x23] = '^',
		[0x24] = '&', [0x25] = '*', [0x26] = '(', [0x27] = ')',
		[0x2C] = ' ', [0x2D] = '_', [0x2E] = '+', [0x2F] = '{',
		[0x30] = '}', [0x31] = '|', [0x33] = ':', [0x34] = '"',
		[0x35] = '~', [0x36] = '<', [0x37] = '>', [0x38] = '?',
	};
	if (code == 0x28) {
		return '\n';
	}
	if (code == 0x2A) {
		return '\b';
	}
	if (code == 0x2B) {
		return '\t';
	}
	if (code == 0x52) {
		return KBD_UP;
	}
	if (code == 0x51) {
		return KBD_DOWN;
	}
	if (code >= sizeof(unshifted)) {
		return 0;
	}
	char ch = shift ? shifted[code] : unshifted[code];
	return (unsigned char)ch;
}

/** Emit keys that appeared in `now` but not in `hid_prev`. */
static void hid_diff(const uint8_t now[8])
{
	int shift = (now[0] & 0x22) != 0;
	for (int i = 2; i < 8; i++) {
		uint8_t code = now[i];
		if (code == 0 || code == 1) {
			continue;
		}
		int seen = 0;
		for (int j = 2; j < 8; j++) {
			if (hid_prev[j] == code) {
				seen = 1;
				break;
			}
		}
		if (seen) {
			continue;
		}
		int key = hid_usage(code, shift);
		if (key) {
			kbd_inject(key);
		}
	}
	memcpy(hid_prev, now, 8);
}

/** Locate a boot keyboard interface and its interrupt IN endpoint. */
static int parse_hid(const uint8_t *cfg, uint16_t total, uint8_t *iface,
	uint8_t *ep, uint8_t *mps)
{
	size_t off = 0;
	int in_kbd = 0;
	int found = 0;
	*ep = 0;
	*mps = 8;
	while (off + 2 <= total) {
		uint8_t len = cfg[off];
		uint8_t typ = cfg[off + 1];
		if (len < 2 || off + len > total) {
			break;
		}
		if (typ == 4 && len >= 9) {
			uint8_t cls = cfg[off + 5];
			uint8_t sub = cfg[off + 6];
			uint8_t proto = cfg[off + 7];
			in_kbd = (cls == 3 && (sub == 1 || sub == 0) && proto == 1);
			if (in_kbd) {
				*iface = cfg[off + 2];
				found = 1;
			}
		} else if (in_kbd && typ == 5 && len >= 7) {
			uint8_t addr = cfg[off + 2];
			uint8_t attr = cfg[off + 3];
			if ((attr & 3) == 3 && (addr & 0x80)) {
				*ep = addr & 0x0F;
				uint16_t pk = (uint16_t)(cfg[off + 4] | (cfg[off + 5] << 8));
				*mps = pk ? (uint8_t)pk : 8;
			}
		}
		off += len;
	}
	return found;
}

/** Queue one interrupt IN TD on ED 1 (periodic list). */
static void hid_arm_int(void)
{
	uint32_t buf = obuf_phys + 128;
	memset(&tds[4], 0, sizeof(tds[4]));
	memset(&tds[5], 0, sizeof(tds[5]));
	tds[4].flags = TD_DP_IN | TD_R | TD_DI_NONE | (0xFu << TD_CC_SHIFT);
	tds[4].cbp = buf;
	tds[4].be = buf + 7;
	tds[4].next = td_phys(5);
	uint32_t flags = hid_addr | ((uint32_t)hid_ep << 7) | ED_IN
		| ((uint32_t)hid_mps << 16);
	if (hid_ls) {
		flags |= ED_LOWSPEED;
	}
	eds[1].flags = flags;
	eds[1].tail = td_phys(5);
	eds[1].head = td_phys(4);
	eds[1].next = 0;
	ohci_sync();
}

/** SET_ADDRESS / SET_CONFIG / boot protocol on one addressed device. */
static int try_hid(uint8_t addr, int ls)
{
	uint8_t dd[18];
	memset(dd, 0, sizeof(dd));
	if (ohci_ctrl(0, 0x80, 6, 0x0100, 0, dd, 18, ls) != 0) {
		return 0;
	}
	if (ohci_ctrl(0, 0x00, 5, addr, 0, NULL, 0, ls) != 0) {
		return 0;
	}
	uint8_t hdr[8];
	memset(hdr, 0, sizeof(hdr));
	if (ohci_ctrl(addr, 0x80, 6, 0x0200, 0, hdr, 8, ls) != 0) {
		return 0;
	}
	uint16_t total = (uint16_t)(hdr[2] | (hdr[3] << 8));
	if (total < 9 || total > 256) {
		return 0;
	}
	uint8_t cfg[256];
	memset(cfg, 0, sizeof(cfg));
	if (ohci_ctrl(addr, 0x80, 6, 0x0200, 0, cfg, total, ls) != 0) {
		return 0;
	}
	uint8_t iface = 0;
	uint8_t ep = 0;
	uint8_t mps = 8;
	if (!parse_hid(cfg, total, &iface, &ep, &mps)) {
		return 0;
	}
	if (ohci_ctrl(addr, 0x00, 9, cfg[5], 0, NULL, 0, ls) != 0) {
		return 0;
	}
	/* SET_PROTOCOL boot, SET_IDLE 0. */
	(void)ohci_ctrl(addr, 0x21, 0x0B, 0, iface, NULL, 0, ls);
	(void)ohci_ctrl(addr, 0x21, 0x0A, 0, iface, NULL, 0, ls);
	hid_addr = addr;
	hid_iface = iface;
	hid_ep = ep;
	hid_mps = mps;
	hid_ls = (uint8_t)ls;
	memset(hid_prev, 0, sizeof(hid_prev));
	hid_ready = 1;
	hid_use_int = 0;
	if (ep != 0) {
		unsigned i;
		for (i = 0; i < 32; i++) {
			hcca->inttable[i] = ed_phys(1);
		}
		hid_arm_int();
		rw(HC_CONTROL, (rr(HC_CONTROL) & ~CTRL_IR)
			| CTRL_USBOPER | CTRL_CLE | CTRL_PLE);
		hid_use_int = 1;
	}
	tty_puts("usb: HID keyboard (OHCI boot protocol)\n");
	return 1;
}

/** Reset an OHCI root port and try HID. */
static int ohci_port(unsigned i, uint8_t *addr)
{
	uint32_t off = HC_RHPORTSTATUS + i * 4;
	uint32_t st = rr(off);
	if ((st & PORT_CCS) == 0) {
		return 0;
	}
	rw(off, PORT_PPS | PORT_CSC);
	uint64_t t0 = pit_ticks();
	while (pit_ticks() - t0 < 20) {
		ohci_idle();
	}
	rw(off, PORT_PRS);
	t0 = pit_ticks();
	while (pit_ticks() - t0 < 100) {
		if (rr(off) & PORT_PRSC) {
			break;
		}
		ohci_idle();
	}
	rw(off, PORT_PRSC);
	t0 = pit_ticks();
	while (pit_ticks() - t0 < 10) {
		ohci_idle();
	}
	st = rr(off);
	if ((st & PORT_PES) == 0 && (st & PORT_CCS) == 0) {
		return 0;
	}
	int ls = (st & PORT_LSDA) != 0;
	if (try_hid(*addr, ls)) {
		return 1;
	}
	(*addr)++;
	return 0;
}

/** Reset one OHCI function and scan its root ports. */
static int ohci_controller(const struct pci_device *dev)
{
	uint64_t bar = pci_mmio_bar(dev, 0);
	if (bar == 0) {
		return 0;
	}
	pci_enable_mem_bm(dev);
	if (!phys_map_mmio(bar, 0x1000)) {
		return 0;
	}
	op = phys_to_virt(bar);
	uint32_t rev = rr(HC_REVISION) & 0xFF;
	if (rev < 0x10) {
		return 0;
	}

	uint32_t ctrl = rr(HC_CONTROL);
	if (ctrl & CTRL_IR) {
		rw(HC_CMDSTATUS, rr(HC_CMDSTATUS) | CMD_OCR);
		uint64_t d = pit_ticks() + 200;
		while (pit_ticks() < d) {
			if ((rr(HC_CONTROL) & CTRL_IR) == 0) {
				break;
			}
			ohci_idle();
		}
	}

	uint32_t fmin = rr(HC_FMINTERVAL);
	rw(HC_CONTROL, 0);
	rw(HC_CMDSTATUS, CMD_HCR);
	uint64_t d = pit_ticks() + 50;
	while (pit_ticks() < d) {
		if ((rr(HC_CMDSTATUS) & CMD_HCR) == 0) {
			break;
		}
		ohci_idle();
	}
	if (fmin == 0) {
		fmin = 0xA7782EDF;
	}
	rw(HC_FMINTERVAL, fmin);
	rw(HC_PERIODICSTART, 0x2A2F);
	memset(hcca, 0, sizeof(*hcca));
	rw(HC_HCCA, hcca_phys);
	rw(HC_INTDISABLE, 0xFFFFFFFFu);
	rw(HC_INTSTATUS, 0xFFFFFFFFu);
	rw(HC_RHSTATUS, 1u << 16);	/* LPSC: power all ports */

	uint32_t ndp = rr(HC_RHDESCA) & 0xFF;
	if (ndp == 0 || ndp > 15) {
		ndp = 2;
	}
	rw(HC_CONTROL, CTRL_USBOPER | CTRL_CLE);
	uint64_t t0 = pit_ticks();
	while (pit_ticks() - t0 < 10) {
		ohci_idle();
	}

	uint8_t addr = 1;
	for (unsigned i = 0; i < ndp; i++) {
		if (ohci_port(i, &addr)) {
			return 1;
		}
	}
	return 0;
}

/** Walk PCI OHCI companions after EHCI has parked FS/LS ports. */
void usb_ohci_init(void (*idle)(void))
{
	idle_fn = idle;
	hid_ready = 0;
	if (hcca == NULL) {
		hcca = phys_alloc(sizeof(*hcca), &hcca_phys);
		eds = phys_alloc(sizeof(struct ohci_ed) * 4, &eds_phys);
		tds = phys_alloc(sizeof(struct ohci_td) * 8, &tds_phys);
		obuf = phys_alloc(256, &obuf_phys);
	}
	if (hcca == NULL || eds == NULL || tds == NULL || obuf == NULL) {
		return;
	}
	for (unsigned nth = 0; nth < 8; nth++) {
		struct pci_device dev;
		if (!pci_find_class(OHCI_CLASS, OHCI_SUB, OHCI_PI, nth, &dev)) {
			break;
		}
		if (ohci_controller(&dev)) {
			return;
		}
	}
}

/** Poll interrupt IN, or GET_REPORT if the device has no INT endpoint. */
void usb_ohci_poll(void)
{
	if (!hid_ready) {
		return;
	}
	if (hid_use_int) {
		uint32_t cc = tds[4].flags >> TD_CC_SHIFT;
		if (cc == 0xF) {
			return;
		}
		if (cc == 0) {
			uint8_t now[8];
			memcpy(now, obuf + 128, 8);
			hid_diff(now);
		}
		hid_arm_int();
		return;
	}
	uint8_t now[8];
	memset(now, 0, 8);
	if (ohci_ctrl(hid_addr, 0xA1, 0x01, 0x0100, hid_iface, now, 8, hid_ls) != 0) {
		return;
	}
	hid_diff(now);
}

void usb_poll(void)
{
	usb_ohci_poll();
}

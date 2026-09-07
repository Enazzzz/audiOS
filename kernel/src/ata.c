#include "ata.h"
#include "audio.h"
#include "io.h"
#include "klib.h"
#include "pci.h"
#include "pit.h"

#define ATA_DATA	0
#define ATA_ERR		1
#define ATA_COUNT	2
#define ATA_LBA0	3
#define ATA_LBA1	4
#define ATA_LBA2	5
#define ATA_DEV		6
#define ATA_CMD		7
#define ATA_STATUS	7

#define ATA_ST_ERR	0x01u
#define ATA_ST_DRQ	0x08u
#define ATA_ST_DF	0x20u
#define ATA_ST_DRDY	0x40u
#define ATA_ST_BSY	0x80u

#define ATA_CMD_IDENTIFY	0xECu
#define ATA_CMD_READ		0x20u
#define ATA_CMD_WRITE		0x30u

#define ATA_PROBE_MS	400u
#define ATA_IO_MS	3000u

static struct ata_dev devs[ATA_MAX_DEV];
static unsigned ndev;
static char last_err[72];

/** ATA PIO needs ~400 ns after a drive select. */
static void ata_pause(void)
{
	io_wait();
	io_wait();
	io_wait();
	io_wait();
}

/** True if this status byte means "nobody is decoding this I/O port". */
static int ata_floating(uint8_t st)
{
	return st == 0xFFu;
}

/** Spin until BSY clears or `ms` elapses. Returns 0 on timeout. */
static int ata_wait_not_bsy(uint16_t io, uint32_t ms)
{
	uint64_t t0 = pit_ticks();
	for (;;) {
		uint8_t st = inb((uint16_t)(io + ATA_STATUS));
		if ((st & ATA_ST_BSY) == 0) {
			return 1;
		}
		audio_service();
		if (pit_ticks() - t0 >= ms) {
			ksnprintf(last_err, sizeof(last_err), "%s", "busy timeout");
			return 0;
		}
	}
}

/** Wait until DRQ is set (and not BSY/ERR). */
static int ata_wait_drq(uint16_t io, uint32_t ms)
{
	uint64_t t0 = pit_ticks();
	for (;;) {
		uint8_t st = inb((uint16_t)(io + ATA_STATUS));
		if ((st & ATA_ST_BSY) == 0) {
			if (st & ATA_ST_ERR) {
				ksnprintf(last_err, sizeof(last_err), "ATA ERR status=0x%02x", st);
				return 0;
			}
			if (st & ATA_ST_DRQ) {
				return 1;
			}
		}
		audio_service();
		if (pit_ticks() - t0 >= ms) {
			ksnprintf(last_err, sizeof(last_err), "%s", "DRQ timeout");
			return 0;
		}
	}
}

/** Select master/slave and wait for the device to accept commands. */
static int ata_select(const struct ata_dev *d)
{
	outb((uint16_t)(d->io + ATA_DEV), (uint8_t)(0xE0u | (d->slave ? 0x10u : 0)));
	ata_pause();
	if (ata_floating(inb((uint16_t)(d->io + ATA_STATUS)))) {
		ksnprintf(last_err, sizeof(last_err), "%s", "floating bus");
		return 0;
	}
	return ata_wait_not_bsy(d->io, ATA_PROBE_MS);
}

/** Issue an LBA28 command for one sector. */
static int ata_issue(const struct ata_dev *d, uint32_t lba, uint8_t cmd)
{
	if (!ata_select(d)) {
		return 0;
	}
	outb((uint16_t)(d->io + ATA_COUNT), 1);
	outb((uint16_t)(d->io + ATA_LBA0), (uint8_t)lba);
	outb((uint16_t)(d->io + ATA_LBA1), (uint8_t)(lba >> 8));
	outb((uint16_t)(d->io + ATA_LBA2), (uint8_t)(lba >> 16));
	outb((uint16_t)(d->io + ATA_DEV),
		(uint8_t)(0xE0u | (d->slave ? 0x10u : 0) | ((lba >> 24) & 0x0Fu)));
	outb((uint16_t)(d->io + ATA_CMD), cmd);
	ata_pause();
	return 1;
}

/** Copy 256 words from the data port. */
static void ata_insw(uint16_t io, uint16_t *dst)
{
	unsigned i;
	for (i = 0; i < 256u; i++) {
		dst[i] = inw(io);
	}
}

/** Copy 256 words to the data port. */
static void ata_outsw(uint16_t io, const uint16_t *src)
{
	unsigned i;
	for (i = 0; i < 256u; i++) {
		outw(io, src[i]);
	}
}

/** ATA model strings store each word byte-swapped. */
static void ata_model_from_id(char *dst, const uint16_t *id)
{
	unsigned i, o = 0;
	for (i = 27; i <= 46; i++) {
		char a = (char)(id[i] >> 8);
		char b = (char)(id[i] & 0xFF);
		if (o + 2u < 44u) {
			dst[o++] = a;
			dst[o++] = b;
		}
	}
	dst[o] = '\0';
	while (o > 0 && dst[o - 1u] == ' ') {
		dst[--o] = '\0';
	}
	i = 0;
	while (dst[i] == ' ') {
		i++;
	}
	if (i) {
		unsigned n = 0;
		while (dst[i]) {
			dst[n++] = dst[i++];
		}
		dst[n] = '\0';
	}
}

static int ata_have(uint16_t io, uint8_t slave)
{
	unsigned i;
	for (i = 0; i < ndev; i++) {
		if (devs[i].io == io && devs[i].slave == slave) {
			return 1;
		}
	}
	return 0;
}

/**
 * IDENTIFY DEVICE on this channel/unit. Returns 0 if empty, ATAPI, or timeout.
 */
static int ata_identify_one(uint16_t io, uint16_t ctrl, uint8_t slave, struct ata_dev *out)
{
	uint16_t id[256];
	uint8_t st;
	uint32_t secs;

	if (ata_have(io, slave)) {
		return 0;
	}
	outb(ctrl, 0x02);	/* nIEN: poll, do not raise IRQ14/15 */
	outb((uint16_t)(io + ATA_DEV), (uint8_t)(0xE0u | (slave ? 0x10u : 0)));
	ata_pause();
	st = inb((uint16_t)(io + ATA_STATUS));
	if (ata_floating(st)) {
		return 0;
	}
	outb((uint16_t)(io + ATA_COUNT), 0);
	outb((uint16_t)(io + ATA_LBA0), 0);
	outb((uint16_t)(io + ATA_LBA1), 0);
	outb((uint16_t)(io + ATA_LBA2), 0);
	outb((uint16_t)(io + ATA_CMD), ATA_CMD_IDENTIFY);
	ata_pause();
	st = inb((uint16_t)(io + ATA_STATUS));
	if (st == 0 || ata_floating(st)) {
		return 0;
	}
	if (!ata_wait_not_bsy(io, ATA_PROBE_MS)) {
		return 0;
	}
	st = inb((uint16_t)(io + ATA_STATUS));
	if ((st & ATA_ST_ERR) || (st & ATA_ST_DRQ) == 0) {
		if (st & ATA_ST_DRQ) {
			uint16_t junk[256];
			ata_insw(io, junk);
		}
		return 0;
	}
	ata_insw(io, id);
	if (id[0] & 0x8000u) {
		return 0;	/* ATAPI */
	}
	secs = (uint32_t)id[60] | ((uint32_t)id[61] << 16);
	if (secs == 0) {
		secs = (uint32_t)id[100] | ((uint32_t)id[101] << 16);
	}
	if (secs < 4u) {
		return 0;
	}
	out->io = io;
	out->ctrl = ctrl;
	out->slave = slave;
	out->sectors = secs;
	ata_model_from_id(out->model, id);
	if (out->model[0] == '\0') {
		ksnprintf(out->model, sizeof(out->model), "%s", "ATA disk");
	}
	return 1;
}

/** Probe master then slave on one command-block pair. */
static void ata_probe_channel(uint16_t io, uint16_t ctrl)
{
	if (ndev >= ATA_MAX_DEV) {
		return;
	}
	if (ata_floating(inb((uint16_t)(io + ATA_STATUS)))) {
		return;
	}
	if (ata_identify_one(io, ctrl, 0, &devs[ndev])) {
		ndev++;
	}
	if (ndev < ATA_MAX_DEV && ata_identify_one(io, ctrl, 1, &devs[ndev])) {
		ndev++;
	}
}

/**
 * Enable I/O on PCI IDE functions and probe native BARs when the chip
 * is not in compatibility mode (A7V333 VIA 0571, SB710, PIIX).
 */
static void ata_probe_pci(void)
{
	unsigned n;
	for (n = 0; n < 8u; n++) {
		struct pci_device d;
		uint8_t pi;
		if (!pci_find_class(0x01, 0x01, 0xFF, n, &d)) {
			break;
		}
		pci_enable_io_bm(&d);
		pi = d.prog_if;
		if (pi & 0x01u) {
			uint16_t io = pci_io_bar(d.bar[0]);
			uint16_t ctrlbar = pci_io_bar(d.bar[1]);
			if (io) {
				/* Native BAR1 is a 4-byte block; device-control is at +2. */
				uint16_t ctrl = ctrlbar ? (uint16_t)(ctrlbar + 2u) : (uint16_t)(io + 0x206);
				ata_probe_channel(io, ctrl);
			}
		}
		if (pi & 0x04u) {
			uint16_t io = pci_io_bar(d.bar[2]);
			uint16_t ctrlbar = pci_io_bar(d.bar[3]);
			if (io) {
				uint16_t ctrl = ctrlbar ? (uint16_t)(ctrlbar + 2u) : (uint16_t)(io + 0x206);
				ata_probe_channel(io, ctrl);
			}
		}
	}
}

void ata_init(void)
{
	ndev = 0;
	last_err[0] = '\0';
	ksnprintf(last_err, sizeof(last_err), "%s", "ok");
	ata_probe_pci();
	ata_probe_channel(0x1F0, 0x3F6);
	ata_probe_channel(0x170, 0x376);
}

unsigned ata_count(void)
{
	return ndev;
}

const struct ata_dev *ata_at(unsigned index)
{
	if (index >= ndev) {
		return 0;
	}
	return &devs[index];
}

int ata_read(unsigned index, uint32_t lba, uint32_t count, void *buf)
{
	uint8_t *p = (uint8_t *)buf;
	const struct ata_dev *d;
	if (index >= ndev || buf == 0) {
		ksnprintf(last_err, sizeof(last_err), "%s", "bad drive");
		return 0;
	}
	d = &devs[index];
	while (count--) {
		if (lba >= d->sectors) {
			ksnprintf(last_err, sizeof(last_err), "%s", "LBA past end");
			return 0;
		}
		if (!ata_issue(d, lba, ATA_CMD_READ) || !ata_wait_drq(d->io, ATA_IO_MS)) {
			return 0;
		}
		ata_insw(d->io, (uint16_t *)p);
		p += 512;
		lba++;
	}
	return 1;
}

int ata_write(unsigned index, uint32_t lba, uint32_t count, const void *buf)
{
	const uint8_t *p = (const uint8_t *)buf;
	const struct ata_dev *d;
	if (index >= ndev || buf == 0) {
		ksnprintf(last_err, sizeof(last_err), "%s", "bad drive");
		return 0;
	}
	d = &devs[index];
	while (count--) {
		if (lba >= d->sectors) {
			ksnprintf(last_err, sizeof(last_err), "%s", "LBA past end");
			return 0;
		}
		if (!ata_issue(d, lba, ATA_CMD_WRITE) || !ata_wait_drq(d->io, ATA_IO_MS)) {
			return 0;
		}
		ata_outsw(d->io, (const uint16_t *)p);
		if (!ata_wait_not_bsy(d->io, ATA_IO_MS)) {
			return 0;
		}
		if (inb((uint16_t)(d->io + ATA_STATUS)) & ATA_ST_ERR) {
			ksnprintf(last_err, sizeof(last_err), "%s", "write ERR");
			return 0;
		}
		p += 512;
		lba++;
	}
	return 1;
}

int ata_flush(unsigned index)
{
	const struct ata_dev *d;
	if (index >= ndev) {
		ksnprintf(last_err, sizeof(last_err), "%s", "bad drive");
		return 0;
	}
	d = &devs[index];
	if (!ata_select(d)) {
		return 0;
	}
	outb((uint16_t)(d->io + ATA_CMD), 0xE7);
	ata_pause();
	return ata_wait_not_bsy(d->io, ATA_IO_MS);
}

const char *ata_error(void)
{
	return last_err;
}

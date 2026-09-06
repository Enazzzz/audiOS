#include "fdc.h"
#include "io.h"
#include "klib.h"
#include "phys.h"
#include "pic.h"
#include "pit.h"

#define FDC_DOR		0x3F2
#define FDC_MSR		0x3F4
#define FDC_FIFO	0x3F5
#define FDC_CCR		0x3F7

#define MSR_RQM		0x80
#define MSR_DIO		0x40
#define MSR_NDMA	0x20
#define MSR_CB		0x10

#define DOR_RESET	0x04
#define DOR_DMA		0x08
#define DOR_MOT0	0x10

#define CMD_SPECIFY	0x03
#define CMD_WRITE	0x45	/* MF | write (single sector) */
#define CMD_READ	0x46	/* MF | read */
#define CMD_RECAL	0x07
#define CMD_SENSEI	0x08
#define CMD_FORMAT	0x4D
#define CMD_SEEK	0x0F
#define CMD_VERSION	0x10
#define CMD_CONFIGURE	0x13

#define DMA_MASK	0x0A
#define DMA_MODE	0x0B
#define DMA_FF		0x0C
#define DMA_ADDR	0x04
#define DMA_COUNT	0x05
#define DMA_PAGE	0x81

static int present;
static volatile int irq_seen;
static uint8_t *dma_virt;
static uint32_t dma_phys;
static char err[80];
static uint8_t dor;
static void (*idle_cb)(void);

/** Pump audio while waiting on the FDC. */
static void pump(void)
{
	if (idle_cb) {
		idle_cb();
	}
}

const char *fdc_error(void)
{
	return err[0] ? err : "ok";
}

int fdc_present(void)
{
	return present;
}

void fdc_irq(void)
{
	irq_seen = 1;
}

/** Millisecond wait with audio refill. */
static void fdc_sleep(uint32_t ms)
{
	uint64_t until = pit_ticks() + ms;
	while (pit_ticks() < until) {
		pump();
		io_wait();
	}
}

static void fail(const char *msg)
{
	ksnprintf(err, sizeof(err), "%s", msg);
}

/** Wait until the FIFO can take a command byte. */
static int wait_rqm_in(uint32_t ms)
{
	uint64_t until = pit_ticks() + ms;
	while (pit_ticks() < until) {
		uint8_t msr = inb(FDC_MSR);
		if (msr == 0xFF) {
			fail("no controller");
			return 0;
		}
		if ((msr & (MSR_RQM | MSR_DIO)) == MSR_RQM) {
			return 1;
		}
		pump();
	}
	fail("fifo timeout (in)");
	return 0;
}

/** Wait until a result byte is available. */
static int wait_rqm_out(uint32_t ms)
{
	uint64_t until = pit_ticks() + ms;
	while (pit_ticks() < until) {
		uint8_t msr = inb(FDC_MSR);
		if ((msr & (MSR_RQM | MSR_DIO)) == (MSR_RQM | MSR_DIO)) {
			return 1;
		}
		pump();
	}
	fail("fifo timeout (out)");
	return 0;
}

static int fifo_put(uint8_t b)
{
	if (!wait_rqm_in(500)) {
		return 0;
	}
	outb(FDC_FIFO, b);
	return 1;
}

static int fifo_get(uint8_t *b)
{
	if (!wait_rqm_out(500)) {
		return 0;
	}
	*b = inb(FDC_FIFO);
	return 1;
}

/** IRQ6 or result-phase poll, whichever comes first. */
static int wait_done(uint32_t ms)
{
	uint64_t until = pit_ticks() + ms;
	while (pit_ticks() < until) {
		if (irq_seen) {
			irq_seen = 0;
			return 1;
		}
		uint8_t msr = inb(FDC_MSR);
		if ((msr & (MSR_RQM | MSR_DIO | MSR_CB)) == (MSR_RQM | MSR_DIO | MSR_CB)) {
			return 1;
		}
		pump();
	}
	fail("irq timeout");
	return 0;
}

static void motor_on(void)
{
	dor = (uint8_t)(DOR_RESET | DOR_DMA | DOR_MOT0);
	outb(FDC_DOR, dor);
	fdc_sleep(400);
}

static void motor_off(void)
{
	dor = (uint8_t)(DOR_RESET | DOR_DMA);
	outb(FDC_DOR, dor);
}

/** Program ISA DMA channel 2. `mode` is 0x46 (read FDC→mem) or 0x4A (write). */
static void dma_setup(uint32_t bytes, uint8_t mode)
{
	uint16_t count = (uint16_t)(bytes - 1u);
	outb(DMA_MASK, 0x06);
	outb(DMA_FF, 0xFF);
	outb(DMA_ADDR, (uint8_t)(dma_phys & 0xFF));
	outb(DMA_ADDR, (uint8_t)((dma_phys >> 8) & 0xFF));
	outb(DMA_PAGE, (uint8_t)((dma_phys >> 16) & 0xFF));
	outb(DMA_FF, 0xFF);
	outb(DMA_COUNT, (uint8_t)(count & 0xFF));
	outb(DMA_COUNT, (uint8_t)((count >> 8) & 0xFF));
	outb(DMA_MODE, mode);
	outb(DMA_MASK, 0x02);
}

static int sense_int(uint8_t *st0, uint8_t *cyl)
{
	uint8_t a, b;
	if (!fifo_put(CMD_SENSEI) || !fifo_get(&a) || !fifo_get(&b)) {
		return 0;
	}
	if (st0) {
		*st0 = a;
	}
	if (cyl) {
		*cyl = b;
	}
	return 1;
}

/** Drain up to 7 result bytes after a data command. */
static int read_result(uint8_t *st0)
{
	uint8_t buf[7];
	unsigned i;
	for (i = 0; i < 7; i++) {
		if (!wait_rqm_out(200)) {
			if (i == 0) {
				return 0;
			}
			break;
		}
		buf[i] = inb(FDC_FIFO);
	}
	if (st0 && i > 0) {
		*st0 = buf[0];
	}
	return 1;
}

static int specify(void)
{
	/* SRT=8ms, HUT=max, HLT=10ms, DMA. */
	return fifo_put(CMD_SPECIFY) && fifo_put(0x80) && fifo_put(0x0A);
}

static int configure(void)
{
	/* Implied seek, FIFO on, poll disable, thresh 8. */
	return fifo_put(CMD_CONFIGURE) && fifo_put(0x00) && fifo_put(0x47) && fifo_put(0x00);
}

static int fdc_reset(void)
{
	unsigned i;
	irq_seen = 0;
	outb(FDC_DOR, 0x00);
	fdc_sleep(4);
	outb(FDC_DOR, (uint8_t)(DOR_RESET | DOR_DMA));
	dor = (uint8_t)(DOR_RESET | DOR_DMA);
	outb(FDC_CCR, 0x00);	/* 500 kb/s */
	if (!wait_done(1000)) {
		/* Some controllers skip the reset IRQ; try sense anyway. */
		irq_seen = 0;
	}
	for (i = 0; i < 4; i++) {
		uint8_t st0 = 0, cyl = 0;
		if (!sense_int(&st0, &cyl)) {
			break;
		}
	}
	if (!specify() || !configure()) {
		return 0;
	}
	return 1;
}

static int recalibrate(void)
{
	uint8_t st0 = 0, cyl = 0;
	motor_on();
	irq_seen = 0;
	if (!fifo_put(CMD_RECAL) || !fifo_put(0x00)) {
		return 0;
	}
	if (!wait_done(3000)) {
		return 0;
	}
	if (!sense_int(&st0, &cyl)) {
		return 0;
	}
	if (cyl != 0 && (st0 & 0x20) == 0) {
		fail("recalibrate");
		return 0;
	}
	return 1;
}

static int seek(uint8_t cyl, uint8_t head)
{
	uint8_t st0 = 0, got = 0;
	irq_seen = 0;
	if (!fifo_put(CMD_SEEK) || !fifo_put(head << 2) || !fifo_put(cyl)) {
		return 0;
	}
	if (!wait_done(3000) || !sense_int(&st0, &got)) {
		return 0;
	}
	(void)st0;
	return 1;
}

static void lba_chs(uint32_t lba, uint8_t *c, uint8_t *h, uint8_t *s)
{
	*s = (uint8_t)((lba % FDC_SPT) + 1u);
	lba /= FDC_SPT;
	*h = (uint8_t)(lba % FDC_HEADS);
	*c = (uint8_t)(lba / FDC_HEADS);
}

static int xfer(uint32_t lba, int write)
{
	uint8_t c, h, s, st0 = 0;
	uint8_t cmd = write ? CMD_WRITE : CMD_READ;
	unsigned try;
	if (!present || lba >= FDC_SECTORS) {
		fail("bad lba");
		return 0;
	}
	lba_chs(lba, &c, &h, &s);
	motor_on();
	for (try = 0; try < 3u; try++) {
		if (!seek(c, h)) {
			continue;
		}
		dma_setup(FDC_SECSZ, write ? 0x4A : 0x46);
		irq_seen = 0;
		if (!fifo_put(cmd) || !fifo_put((uint8_t)(h << 2)) || !fifo_put(c)
			|| !fifo_put(h) || !fifo_put(s) || !fifo_put(0x02)
			|| !fifo_put(FDC_SPT) || !fifo_put(0x1B) || !fifo_put(0xFF)) {
			continue;
		}
		if (!wait_done(2000) || !read_result(&st0)) {
			continue;
		}
		if (st0 & 0xC0) {
			fail(write ? "write failed" : "read failed");
			continue;
		}
		return 1;
	}
	return 0;
}

int fdc_read(uint32_t lba, void *buf)
{
	if (!xfer(lba, 0)) {
		return 0;
	}
	memcpy(buf, dma_virt, FDC_SECSZ);
	return 1;
}

int fdc_write(uint32_t lba, const void *buf)
{
	memcpy(dma_virt, buf, FDC_SECSZ);
	return xfer(lba, 1);
}

int fdc_format_disk(void (*idle)(void))
{
	uint8_t cyl, head;
	idle_cb = idle;
	if (!present) {
		fail("no floppy controller");
		return 0;
	}
	if (!recalibrate()) {
		return 0;
	}
	for (cyl = 0; cyl < FDC_CYLS; cyl++) {
		for (head = 0; head < FDC_HEADS; head++) {
			unsigned i;
			uint8_t st0 = 0;
			if (!seek(cyl, head)) {
				return 0;
			}
			/* Per-sector CHRN table the FDC writes into the gap. */
			for (i = 0; i < FDC_SPT; i++) {
				dma_virt[i * 4u + 0u] = cyl;
				dma_virt[i * 4u + 1u] = head;
				dma_virt[i * 4u + 2u] = (uint8_t)(i + 1u);
				dma_virt[i * 4u + 3u] = 0x02;	/* 512 bytes */
			}
			dma_setup(FDC_SPT * 4u, 0x4A);
			irq_seen = 0;
			if (!fifo_put(CMD_FORMAT) || !fifo_put((uint8_t)(head << 2))
				|| !fifo_put(0x02) || !fifo_put(FDC_SPT)
				|| !fifo_put(0x54) || !fifo_put(0xF6)) {
				return 0;
			}
			if (!wait_done(3000) || !read_result(&st0)) {
				return 0;
			}
			if (st0 & 0xC0) {
				fail("format failed");
				return 0;
			}
			pump();
		}
	}
	motor_off();
	err[0] = '\0';
	return 1;
}

void fdc_init(void (*idle)(void))
{
	uint8_t ver = 0;
	present = 0;
	err[0] = '\0';
	irq_seen = 0;
	idle_cb = idle;
	dma_virt = phys_alloc_isa(FDC_SECSZ, &dma_phys);
	if (dma_virt == NULL || dma_phys == 0) {
		fail("no ISA DMA buffer");
		return;
	}
	if (inb(FDC_MSR) == 0xFF && inb(FDC_DOR) == 0xFF) {
		fail("no floppy controller");
		return;
	}
	pic_unmask(6);
	if (!fdc_reset()) {
		return;
	}
	if (!fifo_put(CMD_VERSION) || !fifo_get(&ver)) {
		/* 82077 talks; an 8272 may not implement VERSION. */
		present = 1;
		err[0] = '\0';
		return;
	}
	present = 1;
	err[0] = '\0';
	(void)ver;
}

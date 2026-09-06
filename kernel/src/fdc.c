#include "fdc.h"
#include "io.h"
#include "klib.h"
#include "phys.h"
#include "pic.h"
#include "pit.h"

#define FDC_DOR		0x3F2
#define FDC_MSR		0x3F4	/* read: MSR. write: DSR */
#define FDC_FIFO	0x3F5
#define FDC_CCR		0x3F7

#define MSR_RQM		0x80
#define MSR_DIO		0x40
#define MSR_NDMA	0x20
#define MSR_CB		0x10

#define DOR_RESET	0x04
#define DOR_DMA		0x08

#define CMD_SPECIFY	0x03
#define CMD_SENSEDRV	0x04
#define CMD_WRITE	0x45	/* MF | write (single sector) */
#define CMD_READ	0x46	/* MF | read */
#define CMD_RECAL	0x07
#define CMD_SENSEI	0x08
#define CMD_FORMAT	0x4D
#define CMD_SEEK	0x0F
#define CMD_VERSION	0x10
#define CMD_CONFIGURE	0x13

/*
 * CONFIGURE byte 2: EIS | EFIFO | POLL | FIFOTHR.
 * POLL=1 disables drive polling. PC 3.5" pin 34 is disk-change, not RDY.
 */
#define CFG_POLL_OFF	0x57	/* implied seek, FIFO, poll off, thresh 8 */

#define DMA_MASK	0x0A
#define DMA_MODE	0x0B
#define DMA_FF		0x0C
#define DMA_ADDR	0x04
#define DMA_COUNT	0x05
#define DMA_PAGE	0x81

static int present;
static int poll_off;
static int calibrated;
static volatile int irq_seen;
static uint8_t *dma_virt;
static uint32_t dma_phys;
static char err[80];
static uint8_t dor;
static uint8_t fdc_cyl;	/* 0xFF = unknown */
static uint8_t unit;	/* 0 or 1 — twisted-cable A: is usually unit 0 */
static int motor_is_on;
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

unsigned fdc_unit(void)
{
	return unit;
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

/** Drive-busy bit in MSR for the selected unit. */
static uint8_t msr_act(void)
{
	return (uint8_t)(1u << unit);
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

/**
 * IRQ6 or result-phase poll, whichever comes first.
 * Used after reset (polling still on) and after R/W/format (result bytes).
 */
static int wait_done(uint32_t ms)
{
	uint64_t until = pit_ticks() + ms;
	uint8_t msr = 0;
	while (pit_ticks() < until) {
		if (irq_seen) {
			irq_seen = 0;
			return 1;
		}
		msr = inb(FDC_MSR);
		if ((msr & (MSR_RQM | MSR_DIO | MSR_CB)) == (MSR_RQM | MSR_DIO | MSR_CB)) {
			return 1;
		}
		pump();
	}
	ksnprintf(err, sizeof(err), "irq timeout (msr=0x%02x)", msr);
	return 0;
}

/**
 * Seek / recalibrate have no result phase.
 *
 * The SB710 photo was `msr=0x81` (RQM|ACTA): FIFO is ready for a command
 * while the unit-busy bit stays set. This Super I/O leaves ACTA set until
 * Sense Interrupt; waiting for the bit to fall deadlocks. Always return so
 * the caller can SIS — even on timeout, even if ACTA is still high.
 *
 * Do not SIS after only a couple of milliseconds: that returns 0x80 and
 * locks the controller (0.3.1's "no-op" shortcut).
 */
static void wait_seek(uint32_t ms)
{
	uint64_t start = pit_ticks();
	uint64_t until = start + ms;
	uint8_t act = msr_act();
	int saw_act = 0;
	uint8_t msr = 0;

	while (pit_ticks() < until) {
		uint64_t slice = pit_ticks() + 1u;
		uint64_t elapsed;
		while (pit_ticks() < slice && pit_ticks() < until) {
			if (irq_seen) {
				irq_seen = 0;
				err[0] = '\0';
				return;
			}
			msr = inb(FDC_MSR);
			if (msr & act) {
				saw_act = 1;
			} else if (saw_act) {
				irq_seen = 0;
				err[0] = '\0';
				return;
			}
			elapsed = pit_ticks() - start;
			/*
			 * QEMU often never raises ACTA and may not IRQ; the
			 * seek is already done (RQM, idle). Do not SIS at
			 * 2 ms — that locked the FX chip (0x80). 40 ms is
			 * long enough for a real ACTA to appear.
			 */
			if (!saw_act && elapsed >= 40u
				&& (msr & (MSR_RQM | MSR_DIO | MSR_CB | act)) == MSR_RQM) {
				irq_seen = 0;
				err[0] = '\0';
				return;
			}
			io_wait();
		}
		pump();
	}
	ksnprintf(err, sizeof(err), "irq timeout (msr=0x%02x)", msr);
	irq_seen = 0;
}

static void motor_on(void)
{
	uint8_t mot = (uint8_t)(0x10u << unit);
	dor = (uint8_t)(DOR_RESET | DOR_DMA | mot | unit);
	outb(FDC_DOR, dor);
	outb(FDC_CCR, 0x00);	/* 500 kb/s while the drive is selected */
	if (!motor_is_on) {
		fdc_sleep(500);
		motor_is_on = 1;
	}
}

static void motor_off(void)
{
	dor = (uint8_t)(DOR_RESET | DOR_DMA);
	outb(FDC_DOR, dor);
	motor_is_on = 0;
}

void fdc_motor_off(void)
{
	motor_off();
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
	/* 0x80 = invalid command. Extra SIS while a seek is live locks the FDC. */
	if (a == 0x80) {
		fail("sense interrupt");
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

int fdc_sense_drive(uint8_t *st3)
{
	uint8_t b;
	int was_on = motor_is_on;

	/*
	 * Slim 3.5" drives (Epson SMD-300) power the WP photointerrupter
	 * from MOTOR ON. An 80 ms blip left ST3.6 set on a writable disk.
	 */
	motor_on();
	if (!fifo_put(CMD_SENSEDRV) || !fifo_put(unit) || !fifo_get(&b)) {
		if (!was_on) {
			motor_off();
		}
		return 0;
	}
	if (st3) {
		*st3 = b;
	}
	if (!was_on) {
		motor_off();
	}
	return 1;
}

static uint8_t last_st1;

/** Drain up to 7 result bytes after a data command. */
static int read_result(uint8_t *st0)
{
	uint8_t buf[7];
	unsigned i;
	last_st1 = 0;
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
	if (i > 1) {
		last_st1 = buf[1];
	}
	return 1;
}

/**
 * ST1 bit 1 (NW) is the FDC refusing a write because the WP pin is
 * asserted. ST3 bit 6 is only a preview of that pin and can lie.
 */
static void fail_rw(int write, uint8_t st0)
{
	if (last_st1 & 0x02) {
		fail("write protected (FDC pin; right-corner tab, not left HD hole)");
		return;
	}
	ksnprintf(err, sizeof(err), "%s st0=0x%02x st1=0x%02x",
		write ? "write failed" : "read failed", st0, last_st1);
}

static int specify(void)
{
	/* Linux 1.44 MB: SRT≈3ms, HUT=max-ish, HLT≈1ms, DMA (not HUT=0). */
	return fifo_put(CMD_SPECIFY) && fifo_put(0xDF) && fifo_put(0x02);
}

/** Implied seek, FIFO on, drive polling off, threshold 8. */
static int configure(void)
{
	if (!fifo_put(CMD_CONFIGURE) || !fifo_put(0x00) || !fifo_put(CFG_POLL_OFF)
		|| !fifo_put(0x00)) {
		return 0;
	}
	poll_off = 1;
	return 1;
}

static int fdc_reset(void)
{
	unsigned i;
	irq_seen = 0;
	calibrated = 0;
	motor_is_on = 0;
	fdc_cyl = 0xFF;
	outb(FDC_DOR, 0x00);
	fdc_sleep(10);
	outb(FDC_DOR, (uint8_t)(DOR_RESET | DOR_DMA));
	dor = (uint8_t)(DOR_RESET | DOR_DMA);
	outb(FDC_MSR, 0x00);	/* DSR: 500 kb/s */
	outb(FDC_CCR, 0x00);
	if (!poll_off) {
		/*
		 * BIOS default is polling on: reset raises 4 IRQs that must
		 * be sensed. After CONFIGURE, reset does not interrupt.
		 */
		if (!wait_done(1000)) {
			irq_seen = 0;
		}
		for (i = 0; i < 4; i++) {
			uint8_t st0 = 0, cyl = 0;
			if (!sense_int(&st0, &cyl)) {
				break;
			}
		}
	} else {
		fdc_sleep(4);
	}
	if (!specify() || !configure()) {
		return 0;
	}
	return 1;
}

/**
 * Recalibrate `unit`. Always issued: Seek-to-0 after reset is a no-op.
 *
 * After wait_seek, always Sense Interrupt — even if MSR still shows ACTA
 * (0x81). That is how the FX board's FDC reports completion without IRQ6.
 */
static int recalibrate_unit(void)
{
	unsigned try;
	motor_is_on = 0;
	motor_on();
	for (try = 0; try < 3u; try++) {
		uint8_t st0 = 0, cyl = 0;
		irq_seen = 0;
		if (!fifo_put(CMD_RECAL) || !fifo_put(unit)) {
			return 0;
		}
		/* 79 steps × ~8ms plus margin. Then SIS even if ACTA stuck. */
		wait_seek(2500);
		if (!sense_int(&st0, &cyl)) {
			fdc_reset();
			motor_on();
			continue;
		}
		if ((st0 & 0xC0) != 0 && (st0 & 0x10) == 0) {
			ksnprintf(err, sizeof(err), "recalibrate st0=0x%02x", st0);
			fdc_reset();
			motor_on();
			continue;
		}
		fdc_cyl = 0;
		calibrated = 1;
		err[0] = '\0';
		return 1;
	}
	if (err[0] == '\0') {
		fail("recalibrate");
	}
	return 0;
}

/** Recalibrate unit 0, then unit 1 (straight cable / DS0 jumper). */
static int recalibrate(void)
{
	uint8_t u;
	char first[80];
	first[0] = '\0';
	for (u = 0; u < 2; u++) {
		unit = u;
		if (recalibrate_unit()) {
			return 1;
		}
		if (u == 0) {
			ksnprintf(first, sizeof(first), "%s", err);
		}
	}
	unit = 0;
	if (first[0]) {
		ksnprintf(err, sizeof(err), "%s", first);
	}
	return 0;
}

/** Seek the selected unit to `cyl`. */
static int seek(uint8_t cyl)
{
	uint8_t st0 = 0, got = 0;
	if (calibrated && fdc_cyl == cyl) {
		return 1;
	}
	irq_seen = 0;
	if (!fifo_put(CMD_SEEK) || !fifo_put(unit) || !fifo_put(cyl)) {
		return 0;
	}
	wait_seek(2500);
	if (!sense_int(&st0, &got)) {
		return 0;
	}
	(void)st0;
	fdc_cyl = got;
	err[0] = '\0';
	return 1;
}

/** Spin up and find track 0 once per motor session. */
static int ready_drive(void)
{
	if (!calibrated) {
		return recalibrate();
	}
	motor_on();
	return 1;
}

static void lba_chs(uint32_t lba, uint8_t *c, uint8_t *h, uint8_t *s)
{
	*s = (uint8_t)((lba % FDC_SPT) + 1u);
	lba /= FDC_SPT;
	*h = (uint8_t)(lba % FDC_HEADS);
	*c = (uint8_t)(lba / FDC_HEADS);
}

/** Head/unit byte for R/W/format. */
static uint8_t hu(uint8_t head)
{
	return (uint8_t)((head << 2) | unit);
}

/** Read or write one sector. ST3 WP is advisory; ST1 NW is the real refusal. */
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
	if (!ready_drive()) {
		return 0;
	}
	for (try = 0; try < 3u; try++) {
		/*
		 * Implied seek is on; do not issue Seek here. Seek-to-current
		 * cylinder is silent (no IRQ) and was the install timeout.
		 */
		dma_setup(FDC_SECSZ, write ? 0x4A : 0x46);
		irq_seen = 0;
		if (!fifo_put(cmd) || !fifo_put(hu(h)) || !fifo_put(c)
			|| !fifo_put(h) || !fifo_put(s) || !fifo_put(0x02)
			|| !fifo_put(FDC_SPT) || !fifo_put(0x1B) || !fifo_put(0xFF)) {
			continue;
		}
		if (!wait_done(2000) || !read_result(&st0)) {
			continue;
		}
		if (st0 & 0xC0) {
			fail_rw(write, st0);
			continue;
		}
		fdc_cyl = c;
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
		motor_off();
		return 0;
	}
	for (cyl = 0; cyl < FDC_CYLS; cyl++) {
		/* FORMAT has no cylinder argument; implied seek cannot help. */
		if (!seek(cyl)) {
			motor_off();
			return 0;
		}
		for (head = 0; head < FDC_HEADS; head++) {
			unsigned i;
			uint8_t st0 = 0;
			for (i = 0; i < FDC_SPT; i++) {
				dma_virt[i * 4u + 0u] = cyl;
				dma_virt[i * 4u + 1u] = head;
				dma_virt[i * 4u + 2u] = (uint8_t)(i + 1u);
				dma_virt[i * 4u + 3u] = 0x02;	/* 512 bytes */
			}
			dma_setup(FDC_SPT * 4u, 0x4A);
			irq_seen = 0;
			if (!fifo_put(CMD_FORMAT) || !fifo_put(hu(head))
				|| !fifo_put(0x02) || !fifo_put(FDC_SPT)
				|| !fifo_put(0x54) || !fifo_put(0xF6)) {
				motor_off();
				return 0;
			}
			if (!wait_done(3000) || !read_result(&st0)) {
				motor_off();
				return 0;
			}
			if (st0 & 0xC0) {
				fail_rw(1, st0);
				motor_off();
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
	poll_off = 0;
	calibrated = 0;
	motor_is_on = 0;
	unit = 0;
	fdc_cyl = 0xFF;
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
	/*
	 * Disable polling before the first reset so we do not wait on four
	 * BIOS-style poll IRQs that this chipset never delivers.
	 */
	if ((inb(FDC_MSR) & (MSR_RQM | MSR_DIO)) == MSR_RQM) {
		(void)configure();
	}
	if (!fdc_reset()) {
		return;
	}
	if (!fifo_put(CMD_VERSION) || !fifo_get(&ver)) {
		present = 1;
		err[0] = '\0';
		return;
	}
	present = 1;
	err[0] = '\0';
	(void)ver;
}

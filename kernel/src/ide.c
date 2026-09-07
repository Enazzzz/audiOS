#include "ide.h"
#include "ata.h"
#include "audio.h"
#include "fat.h"
#include "fs.h"
#include "klib.h"
#include "tty.h"

#define SLAVE_BIN	"C:/boot/slave.bin"
#define SLAVE_MBR	"C:/boot/slave.mbr"
#define SLAVE_LBA_OFF	0x1ACu
#define SLAVE_NSEC_OFF	0x1B0u

/** Sectors cleared at LBA 0 on `ide format` (1 MiB). Kills old MBRs/FATs. */
#define IDE_WIPE_HEAD	2048u
/** Sectors cleared at the end of the disk (GPT backup header). */
#define IDE_WIPE_TAIL	34u

/** True if sector 0 looks like the FX USB system image (do not overwrite). */
static int ide_is_fx_system(const uint8_t *sec)
{
	unsigned i;
	if (memcmp(sec + 0x47, "AUDIOS", 6) == 0) {
		return 1;
	}
	for (i = 0; i + 6u < 512u; i++) {
		if ((sec[i] == 'L' || sec[i] == 'l')
			&& (sec[i + 1u] == 'I' || sec[i + 1u] == 'i')
			&& (sec[i + 2u] == 'M' || sec[i + 2u] == 'm')
			&& (sec[i + 3u] == 'I' || sec[i + 3u] == 'i')
			&& (sec[i + 4u] == 'N' || sec[i + 4u] == 'n')
			&& (sec[i + 5u] == 'E' || sec[i + 5u] == 'e')) {
			return 1;
		}
	}
	return 0;
}

/** Parse an optional drive index (`ide format 1`). */
static int ide_index(int argc, char **argv, unsigned *out)
{
	unsigned i = 0;
	if (argc >= 3) {
		const char *p = argv[2];
		if (p[0] < '0' || p[0] > '9' || p[1]) {
			tty_puts("ide format [index]\n");
			return 0;
		}
		i = (unsigned)(p[0] - '0');
	}
	if (i >= ata_count()) {
		tty_set_color(TTY_COL_ERR);
		tty_puts("no such IDE HDD\n");
		tty_set_color(TTY_COL_FG);
		return 0;
	}
	*out = i;
	return 1;
}

/** Print detected IDE HDDs and whether C: has the slave payload. */
static void ide_status(void)
{
	unsigned i;
	struct fat_info inf;
	tty_puts("IDE HDD  (format the disk, install the 32-bit A7V333 slave OS)\n");
	if (ata_count() == 0) {
		tty_set_color(TTY_COL_DIM);
		tty_puts("  no HDD on 0x1F0/0x170. Plug a drive into the FX IDE header.\n");
		tty_set_color(TTY_COL_FG);
	}
	for (i = 0; i < ata_count(); i++) {
		const struct ata_dev *d = ata_at(i);
		tty_printf("  %u  IDE 0x%x %s  %s  %u MiB\n",
			i, (unsigned)d->io,
			d->slave ? "slave" : "master",
			d->model,
			(unsigned)(d->sectors / 2048u));
	}
	fat_select(FAT_VOL_SYS);
	if (fat_stat("/boot/slave.bin", &inf) && inf.kind == FAT_FILE) {
		tty_printf("  image: C:/boot/slave.bin  %u bytes\n", inf.size);
	} else {
		tty_set_color(TTY_COL_DIM);
		tty_puts("  image: missing C:/boot/slave.bin (host `make`)\n");
		tty_set_color(TTY_COL_FG);
	}
	if (strcmp(ata_error(), "ok") != 0 && ata_error()[0]) {
		tty_printf("  last error: %s\n", ata_error());
	}
}

/**
 * Write zeros from `lba` for `count` sectors. `audio_service` stays alive.
 * Returns 0 on the first failed write.
 */
static int ide_zero(unsigned index, uint32_t lba, uint32_t count)
{
	uint8_t z[512];
	uint32_t i;
	memset(z, 0, 512);
	for (i = 0; i < count; i++) {
		if (!ata_write(index, lba + i, 1, z)) {
			tty_set_color(TTY_COL_ERR);
			tty_printf("format write lba %u: %s\n", lba + i, ata_error());
			tty_set_color(TTY_COL_FG);
			return 0;
		}
		audio_service();
		if ((i & 127u) == 127u) {
			tty_printf("\r  cleared %u/%u", i + 1u, count);
		}
	}
	if (count >= 128u) {
		tty_puts("\n");
	}
	return 1;
}

/**
 * Clear old boot records: 1 MiB at LBA 0, and the GPT backup at the tail.
 * Does not walk the whole platter (PIO on a large HDD would take hours).
 */
static int ide_format_disk(unsigned index, const struct ata_dev *d)
{
	uint32_t head = IDE_WIPE_HEAD;
	uint32_t tail = 0;
	if (head > d->sectors) {
		head = d->sectors;
	}
	tty_printf("formatting IDE HDD %s (%u MiB)...\n",
		d->model, (unsigned)(d->sectors / 2048u));
	if (!ide_zero(index, 0, head)) {
		return 0;
	}
	if (d->sectors > head + IDE_WIPE_TAIL) {
		tail = IDE_WIPE_TAIL;
		if (!ide_zero(index, d->sectors - tail, tail)) {
			return 0;
		}
	}
	(void)ata_flush(index);
	tty_puts("format ok\n");
	return 1;
}

/** Patch the slave MBR: kernel LBA/count and one active partition for the HDD. */
static void ide_patch_mbr(uint8_t *mbr, uint32_t ksec, uint32_t part_sec)
{
	uint8_t *p = mbr + 0x1BE;
	unsigned z;
	mbr[SLAVE_LBA_OFF] = 1;
	mbr[SLAVE_LBA_OFF + 1u] = 0;
	mbr[SLAVE_LBA_OFF + 2u] = 0;
	mbr[SLAVE_LBA_OFF + 3u] = 0;
	mbr[SLAVE_NSEC_OFF] = (uint8_t)ksec;
	mbr[SLAVE_NSEC_OFF + 1u] = (uint8_t)(ksec >> 8);
	mbr[510] = 0x55;
	mbr[511] = 0xAA;
	for (z = 0; z < 64u; z++) {
		p[z] = 0;
	}
	/* Active partition covering the HDD so Award BIOS lists it as bootable. */
	p[0] = 0x80;
	p[1] = 0x00;
	p[2] = 0x02;
	p[3] = 0x00;
	p[4] = 0x06;
	p[5] = 0xFE;
	p[6] = 0xFF;
	p[7] = 0xFF;
	p[8] = 1;
	p[12] = (uint8_t)part_sec;
	p[13] = (uint8_t)(part_sec >> 8);
	p[14] = (uint8_t)(part_sec >> 16);
	p[15] = (uint8_t)(part_sec >> 24);
}

/**
 * Format (optional) the IDE HDD and install the 32-bit slave OS onto it.
 */
static int ide_put_os(unsigned index, int do_format)
{
	uint8_t mbr[512];
	uint8_t sec[512];
	uint8_t check[512];
	struct fat_info inf;
	uint32_t kbytes = 0;
	uint32_t nsec, lba, got, part_sec;
	const struct ata_dev *d = ata_at(index);

	if (!d) {
		return 0;
	}
	if (!ata_read(index, 0, 1, sec)) {
		tty_set_color(TTY_COL_ERR);
		tty_printf("ide read: %s\n", ata_error());
		tty_set_color(TTY_COL_FG);
		return 0;
	}
	if (ide_is_fx_system(sec)) {
		tty_set_color(TTY_COL_ERR);
		tty_puts("this HDD looks like the FX system image (AUDIOS/Limine).\n");
		tty_puts("refusing to overwrite it. Use a blank IDE HDD for the A7V333.\n");
		tty_set_color(TTY_COL_FG);
		return 0;
	}
	if (!fs_ready()) {
		tty_set_color(TTY_COL_ERR);
		tty_puts("C: is not mounted; cannot read slave.bin\n");
		tty_set_color(TTY_COL_FG);
		return 0;
	}
	fat_select(FAT_VOL_SYS);
	if (!fat_stat("/boot/slave.mbr", &inf) || inf.kind != FAT_FILE || inf.size < 512u) {
		tty_set_color(TTY_COL_ERR);
		tty_puts("C:/boot/slave.mbr missing. Flash a current audios.img.\n");
		tty_set_color(TTY_COL_FG);
		return 0;
	}
	if (!fat_stat("/boot/slave.bin", &inf) || inf.kind != FAT_FILE || inf.size < 512u) {
		tty_set_color(TTY_COL_ERR);
		tty_puts("C:/boot/slave.bin missing. Flash a current audios.img.\n");
		tty_set_color(TTY_COL_FG);
		return 0;
	}
	kbytes = inf.size;
	nsec = (kbytes + 511u) / 512u;
	if (1u + nsec + 2u > d->sectors) {
		tty_set_color(TTY_COL_ERR);
		tty_puts("HDD too small for the slave kernel\n");
		tty_set_color(TTY_COL_FG);
		return 0;
	}
	if (do_format && !ide_format_disk(index, d)) {
		return 0;
	}
	if (!fs_read_at(SLAVE_MBR, 0, mbr, 512, &got) || got != 512) {
		tty_puts("read slave.mbr failed\n");
		return 0;
	}
	part_sec = d->sectors - 1u;
	ide_patch_mbr(mbr, nsec, part_sec);
	tty_printf("installing audiOS slave on IDE HDD (%u kernel sectors)...\n", nsec);
	if (!ata_write(index, 0, 1, mbr)) {
		tty_set_color(TTY_COL_ERR);
		tty_printf("MBR write: %s\n", ata_error());
		tty_set_color(TTY_COL_FG);
		return 0;
	}
	for (lba = 0; lba < nsec; lba++) {
		memset(sec, 0, 512);
		if (!fs_read_at(SLAVE_BIN, lba * 512u, sec, 512, &got) || got == 0) {
			tty_set_color(TTY_COL_ERR);
			tty_printf("read slave.bin lba %u failed\n", lba);
			tty_set_color(TTY_COL_FG);
			return 0;
		}
		if (!ata_write(index, lba + 1u, 1, sec)) {
			tty_set_color(TTY_COL_ERR);
			tty_printf("write lba %u: %s\n", lba + 1u, ata_error());
			tty_set_color(TTY_COL_FG);
			return 0;
		}
		audio_service();
		if ((lba & 15u) == 15u) {
			tty_printf("\r  %u/%u", lba + 1u, nsec);
		}
	}
	tty_puts("\n");
	if (!ata_read(index, 0, 1, check) || check[510] != 0x55 || check[511] != 0xAA) {
		tty_set_color(TTY_COL_ERR);
		tty_puts("verify failed (no 0xAA55 on LBA 0)\n");
		tty_set_color(TTY_COL_FG);
		return 0;
	}
	if (!ata_read(index, 1, 1, check) || (check[0] == 0 && check[1] == 0)) {
		tty_set_color(TTY_COL_ERR);
		tty_puts("verify failed (kernel missing at LBA 1)\n");
		tty_set_color(TTY_COL_FG);
		return 0;
	}
	(void)ata_flush(index);
	tty_set_color(TTY_COL_AUDIO);
	tty_puts("IDE HDD has audiOS slave. Move this disk to the A7V333 and boot it.\n");
	tty_set_color(TTY_COL_FG);
	return 1;
}

void ide_cmd(int argc, char **argv)
{
	const char *sub = (argc > 1) ? argv[1] : "";
	unsigned idx = 0;
	if (sub[0] == '\0' || strcmp(sub, "status") == 0) {
		ide_status();
		return;
	}
	if (ata_count() == 0) {
		tty_set_color(TTY_COL_ERR);
		tty_puts("no IDE HDD. Plug a drive into the FX IDE header (0x1F0).\n");
		tty_set_color(TTY_COL_FG);
		return;
	}
	if (strcmp(sub, "format") == 0 || strcmp(sub, "install") == 0) {
		if (!ide_index(argc, argv, &idx)) {
			return;
		}
		ide_put_os(idx, strcmp(sub, "format") == 0);
		return;
	}
	tty_set_color(TTY_COL_ERR);
	tty_puts("ide [status|format|install] [index]\n");
	tty_set_color(TTY_COL_FG);
}

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
		tty_puts("no such IDE drive\n");
		tty_set_color(TTY_COL_FG);
		return 0;
	}
	*out = i;
	return 1;
}

/** Print detected PATA units and whether C: has the slave payload. */
static void ide_status(void)
{
	unsigned i;
	struct fat_info inf;
	tty_puts("IDE / PATA  (installs the 32-bit A7V333 slave OS)\n");
	if (ata_count() == 0) {
		tty_set_color(TTY_COL_DIM);
		tty_puts("  no drive on 0x1F0/0x170. Plug a disk into the FX IDE header.\n");
		tty_set_color(TTY_COL_FG);
	}
	for (i = 0; i < ata_count(); i++) {
		const struct ata_dev *d = ata_at(i);
		tty_printf("  %u  0x%x %s  %s  %u MiB\n",
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
 * Write the slave MBR + kernel onto drive `index`.
 * Erases only the first N+1 sectors, not the whole disk.
 */
static int ide_install(unsigned index)
{
	uint8_t mbr[512];
	uint8_t sec[512];
	uint8_t check[512];
	struct fat_info inf;
	uint32_t kbytes = 0;
	uint32_t nsec, lba, got;
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
		tty_puts("this disk looks like the FX system image (AUDIOS/Limine).\n");
		tty_puts("refusing to overwrite it. Use a blank IDE disk for the A7V333.\n");
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
		tty_puts("disk too small for the slave kernel\n");
		tty_set_color(TTY_COL_FG);
		return 0;
	}
	if (!fs_read_at(SLAVE_MBR, 0, mbr, 512, &got) || got != 512) {
		tty_puts("read slave.mbr failed\n");
		return 0;
	}
	mbr[SLAVE_LBA_OFF] = 1;
	mbr[SLAVE_LBA_OFF + 1u] = 0;
	mbr[SLAVE_LBA_OFF + 2u] = 0;
	mbr[SLAVE_LBA_OFF + 3u] = 0;
	mbr[SLAVE_NSEC_OFF] = (uint8_t)nsec;
	mbr[SLAVE_NSEC_OFF + 1u] = (uint8_t)(nsec >> 8);
	mbr[510] = 0x55;
	mbr[511] = 0xAA;
	/* Active DOS-type partition so Award BIOS lists the disk as bootable. */
	{
		uint8_t *p = mbr + 0x1BE;
		unsigned z;
		for (z = 0; z < 64u; z++) {
			p[z] = 0;
		}
		p[0] = 0x80;
		p[1] = 0x00;
		p[2] = 0x02;
		p[3] = 0x00;
		p[4] = 0x06;
		p[5] = 0xFE;
		p[6] = 0xFF;
		p[7] = 0xFF;
		p[8] = 1;
		p[12] = (uint8_t)nsec;
		p[13] = (uint8_t)(nsec >> 8);
		p[14] = (uint8_t)(nsec >> 16);
		p[15] = (uint8_t)(nsec >> 24);
	}
	tty_printf("writing A7V333 slave OS to %s (%u sectors)...\n", d->model, nsec + 1u);
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
	(void)ata_flush(index);
	tty_set_color(TTY_COL_AUDIO);
	tty_puts("IDE has audiOS slave. Move this disk to the A7V333 and boot it.\n");
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
		tty_puts("no IDE drive. The FX board's PATA header is 0x1F0.\n");
		tty_set_color(TTY_COL_FG);
		return;
	}
	if (strcmp(sub, "format") == 0 || strcmp(sub, "install") == 0) {
		if (!ide_index(argc, argv, &idx)) {
			return;
		}
		if (strcmp(sub, "format") == 0) {
			tty_puts("writing slave MBR+kernel (start of disk only)...\n");
		}
		ide_install(idx);
		return;
	}
	tty_set_color(TTY_COL_ERR);
	tty_puts("ide [status|format|install] [index]\n");
	tty_set_color(TTY_COL_FG);
}

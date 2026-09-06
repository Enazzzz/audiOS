#include "floppy.h"
#include "audio.h"
#include "fdc.h"
#include "fat.h"
#include "fs.h"
#include "klib.h"
#include "tty.h"

#define FLOPPY_IMG	"C:/boot/floppy.img"
#define FLOPPY_BYTES	(FDC_SECTORS * FDC_SECSZ)

/** Print status: controller, media, whether C: has the Limine image. */
static void floppy_status(void)
{
	struct fat_info inf;
	tty_puts("A:  1.44 MB floppy (80/2/18)\n");
	if (!fdc_present()) {
		tty_set_color(TTY_COL_DIM);
		tty_puts("  controller: none (ISA 0x3F0). USB floppy: write audios.flp on the host.\n");
		tty_set_color(TTY_COL_FG);
		return;
	}
	tty_printf("  controller: 82077 at 0x3F0  DMA ch2  IRQ6  unit %u\n", fdc_unit());
	{
		uint8_t st3 = 0;
		if (fdc_sense_drive(&st3)) {
			tty_printf("  ST3=0x%02x", st3);
			if (st3 & 0x10) {
				tty_puts(" trk0");
			}
			if (st3 & 0x20) {
				tty_puts(" rdy");
			}
			if (st3 & 0x40) {
				tty_puts(" wp");
			}
			tty_puts("\n");
			if (st3 & 0x40) {
				tty_set_color(TTY_COL_ERR);
				tty_puts("  write-protected. Slide the 3.5\" tab so the hole is closed.\n");
				tty_puts("  If the hole is already closed: clean the SMD-300 WP sensor.\n");
				tty_set_color(TTY_COL_FG);
			}
		}
	}
	if (strcmp(fdc_error(), "ok") != 0) {
		tty_printf("  last error: %s\n", fdc_error());
	}
	fat_select(FAT_VOL_SYS);
	if (fat_stat("/boot/floppy.img", &inf) && inf.kind == FAT_FILE) {
		tty_printf("  image: C:/boot/floppy.img  %u bytes\n", inf.size);
	} else {
		tty_set_color(TTY_COL_DIM);
		tty_puts("  image: missing C:/boot/floppy.img (host `make floppy`)\n");
		tty_set_color(TTY_COL_FG);
	}
}

/** Copy the prebuilt Limine floppy image from C: onto A:, 512 bytes at a time. */
static int floppy_write_image(void (*idle)(void))
{
	uint32_t lba;
	uint8_t sec[FDC_SECSZ];
	struct fat_info inf;

	if (!fs_ready()) {
		tty_set_color(TTY_COL_ERR);
		tty_puts("C: is not mounted; cannot read floppy.img\n");
		tty_set_color(TTY_COL_FG);
		return 0;
	}
	fat_select(FAT_VOL_SYS);
	if (!fat_stat("/boot/floppy.img", &inf) || inf.kind != FAT_FILE) {
		tty_set_color(TTY_COL_ERR);
		tty_puts("C:/boot/floppy.img missing. Flash a current audios.img or copy audios.flp.\n");
		tty_set_color(TTY_COL_FG);
		return 0;
	}
	if (inf.size < FLOPPY_BYTES) {
		tty_set_color(TTY_COL_ERR);
		tty_printf("floppy.img is %u bytes, need %u\n", inf.size, FLOPPY_BYTES);
		tty_set_color(TTY_COL_FG);
		return 0;
	}
	tty_puts("writing Limine floppy (1.44 MB)...\n");
	for (lba = 0; lba < FDC_SECTORS; lba++) {
		uint32_t n = 0;
		if (!fs_read_at(FLOPPY_IMG, lba * FDC_SECSZ, sec, FDC_SECSZ, &n) || n != FDC_SECSZ) {
			fdc_motor_off();
			tty_set_color(TTY_COL_ERR);
			tty_printf("read C:/boot/floppy.img lba %u failed\n", lba);
			tty_set_color(TTY_COL_FG);
			return 0;
		}
		if (!fdc_write(lba, sec)) {
			fdc_motor_off();
			tty_set_color(TTY_COL_ERR);
			tty_printf("A: write lba %u: %s\n", lba, fdc_error());
			tty_set_color(TTY_COL_FG);
			return 0;
		}
		if (idle) {
			idle();
		}
		if ((lba % FDC_SPT) == (FDC_SPT - 1u)) {
			tty_printf("\r  track %u/%u", (lba / FDC_SPT) + 1u, FDC_CYLS * FDC_HEADS);
		}
	}
	tty_puts("\n");
	if (!fdc_read(0, sec) || sec[510] != 0x55 || sec[511] != 0xAA) {
		fdc_motor_off();
		tty_set_color(TTY_COL_ERR);
		tty_puts("verify failed (no 0xAA55 on sector 0)\n");
		tty_set_color(TTY_COL_FG);
		return 0;
	}
	fdc_motor_off();
	tty_set_color(TTY_COL_AUDIO);
	tty_puts("A: has Limine. Boot the old BIOS from the floppy; USB stick is C:/D:.\n");
	tty_set_color(TTY_COL_FG);
	return 1;
}

void floppy_cmd(int argc, char **argv)
{
	const char *sub = (argc > 1) ? argv[1] : "";
	if (sub[0] == '\0' || strcmp(sub, "status") == 0) {
		floppy_status();
		return;
	}
	if (!fdc_present()) {
		tty_set_color(TTY_COL_ERR);
		tty_puts("no floppy controller. On Windows: .\\tools\\write-floppy.ps1\n");
		tty_set_color(TTY_COL_FG);
		return;
	}
	if (strcmp(sub, "format") == 0) {
		tty_puts("low-level format 80x2x18 (erases the disk)...\n");
		if (!fdc_format_disk(audio_service)) {
			tty_set_color(TTY_COL_ERR);
			tty_printf("format: %s\n", fdc_error());
			tty_set_color(TTY_COL_FG);
			return;
		}
		tty_puts("format ok. installing Limine chainloader...\n");
		floppy_write_image(audio_service);
		return;
	}
	if (strcmp(sub, "install") == 0 || strcmp(sub, "sys") == 0 || strcmp(sub, "limine") == 0) {
		floppy_write_image(audio_service);
		return;
	}
	tty_set_color(TTY_COL_ERR);
	tty_puts("floppy [status|format|install]\n");
	tty_set_color(TTY_COL_FG);
}

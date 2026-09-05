#include "fs.h"
#include "fat.h"
#include "klib.h"
#include "phys.h"
#include "tty.h"
#include "usb.h"

#include <stddef.h>

static struct blkdev disk;
static struct blkdev extra;
static int ready;
static char errbuf[80];
static uint8_t *file_buf;
static uint32_t file_buf_cap;
static void (*idle_fn)(void);

enum { DRV_C = 0, DRV_D = 1, DRV_E = 2, DRV_N = 3 };

static int cur_drive;
static char drive_cwd[DRV_N][FAT_PATH_MAX];

/** FAT volume for a DOS drive letter index. */
static enum fat_vol_id drive_vol(int d)
{
	if (d == DRV_D) {
		return FAT_VOL_USR;
	}
	if (d == DRV_E) {
		return FAT_VOL_EXT;
	}
	return FAT_VOL_SYS;
}

/** True when that letter has a mounted FAT. */
static int drive_ready(int d)
{
	if (d < 0 || d >= DRV_N) {
		return 0;
	}
	return fat_vol_ready(drive_vol(d)) ? 1 : 0;
}

/** `C:/audio` or `C:/` into `out`. `volpath` is `/` or `/audio`. */
static void format_abs(int drv, const char *volpath, char *out, size_t n)
{
	if (volpath == NULL || volpath[0] == '\0' || strcmp(volpath, "/") == 0) {
		ksnprintf(out, n, "%c:/", (char)('C' + drv));
		return;
	}
	ksnprintf(out, n, "%c:%s", (char)('C' + drv), volpath);
}

/** Parse `C:` / `c:/foo` / `D:\bar`. Returns 1 and sets *drv / *rest. */
static int parse_drive(const char *s, int *drv, const char **rest)
{
	if (s == NULL || s[0] == '\0' || s[1] != ':') {
		return 0;
	}
	char letter = s[0];
	if (letter >= 'a' && letter <= 'z') {
		letter = (char)(letter - 'a' + 'A');
	}
	if (letter < 'C' || letter > 'E') {
		return 0;
	}
	*drv = letter - 'C';
	*rest = s + 2;
	return 1;
}

int fs_is_drive(const char *token)
{
	int d = 0;
	const char *rest = NULL;
	if (!parse_drive(token, &d, &rest)) {
		return 0;
	}
	return rest == NULL || rest[0] == '\0' || strcmp(rest, "/") == 0 || strcmp(rest, "\\") == 0;
}

/** Require a mounted volume; print `errbuf` otherwise. */
static int need_fs(void)
{
	if (ready && fat_mounted()) {
		return 1;
	}
	tty_set_color(TTY_COL_ERR);
	tty_printf("fs: %s\n", errbuf[0] ? errbuf : fs_error());
	tty_set_color(TTY_COL_FG);
	return 0;
}

/** Build an absolute `C:/path` from `rel` into `out`. */
static int abs_path(const char *rel, char *out, size_t n)
{
	char raw[FAT_PATH_MAX];
	int drv = cur_drive;
	const char *rest = rel;
	if (rel == NULL || rel[0] == '\0') {
		format_abs(cur_drive, drive_cwd[cur_drive], out, n);
		return 1;
	}
	char norm[FAT_PATH_MAX];
	unsigned ni = 0;
	for (const char *s = rel; *s && ni + 1 < sizeof(norm); s++) {
		norm[ni++] = (*s == '\\') ? '/' : *s;
	}
	norm[ni] = '\0';
	if (parse_drive(norm, &drv, &rest)) {
		if (rest[0] == '\0') {
			format_abs(drv, drive_cwd[drv], out, n);
			return 1;
		}
		if ((rest[0] == '/' || rest[0] == '\\') && rest[1] == '\0') {
			ksnprintf(raw, sizeof(raw), "/");
		} else if (rest[0] == '/' || rest[0] == '\\') {
			ksnprintf(raw, sizeof(raw), "%s", rest);
			if (raw[0] == '\\') {
				raw[0] = '/';
			}
		} else {
			ksnprintf(raw, sizeof(raw), "/%s", rest);
		}
	} else if (strcmp(norm, "/os") == 0 || str_starts(norm, "/os/")) {
		drv = DRV_C;
		if (strcmp(norm, "/os") == 0) {
			ksnprintf(raw, sizeof(raw), "/");
		} else {
			ksnprintf(raw, sizeof(raw), "%s", norm + 3);
		}
	} else if (norm[0] == '/') {
		drv = fat_vol_ready(FAT_VOL_USR) ? DRV_D : DRV_C;
		ksnprintf(raw, sizeof(raw), "%s", norm);
	} else if (strcmp(drive_cwd[cur_drive], "/") == 0) {
		drv = cur_drive;
		ksnprintf(raw, sizeof(raw), "/%s", norm);
	} else {
		drv = cur_drive;
		ksnprintf(raw, sizeof(raw), "%s/%s", drive_cwd[cur_drive], norm);
	}

	char parts[16][FAT_NAME_MAX];
	unsigned np = 0;
	const char *p = raw;
	while (*p) {
		while (*p == '/') {
			p++;
		}
		if (*p == '\0') {
			break;
		}
		char tok[FAT_NAME_MAX];
		unsigned t = 0;
		while (*p && *p != '/') {
			if (t + 1 < sizeof(tok)) {
				tok[t++] = *p;
			}
			p++;
		}
		tok[t] = '\0';
		if (strcmp(tok, ".") == 0) {
			continue;
		}
		if (strcmp(tok, "..") == 0) {
			if (np > 0) {
				np--;
			}
			continue;
		}
		if (np >= 16) {
			return 0;
		}
		ksnprintf(parts[np++], FAT_NAME_MAX, "%s", tok);
	}
	if (np == 0) {
		format_abs(drv, "/", out, n);
		return 1;
	}
	char vol[FAT_PATH_MAX];
	vol[0] = '\0';
	for (unsigned i = 0; i < np; i++) {
		size_t used = strlen(vol);
		ksnprintf(vol + used, sizeof(vol) - used, "/%s", parts[i]);
	}
	/* Historical `D:/os` (and `cd os` from D:) is the system volume. */
	if (drv == DRV_D) {
		if (strcmp(vol, "/os") == 0) {
			drv = DRV_C;
			ksnprintf(vol, sizeof(vol), "/");
		} else if (str_starts(vol, "/os/")) {
			char rest[FAT_PATH_MAX];
			ksnprintf(rest, sizeof(rest), "%s", vol + 3);
			drv = DRV_C;
			ksnprintf(vol, sizeof(vol), "%s", rest);
		}
	}
	format_abs(drv, vol, out, n);
	return 1;
}

/** Split `C:/audio` into drive index and on-disk `/audio`. */
static int split_abs(const char *abs, int *drv, char *fatpath, size_t n)
{
	const char *rest = NULL;
	if (!parse_drive(abs, drv, &rest)) {
		return 0;
	}
	if (rest[0] == '\0' || ((rest[0] == '/' || rest[0] == '\\') && rest[1] == '\0')) {
		ksnprintf(fatpath, n, "/");
	} else if (rest[0] == '/' || rest[0] == '\\') {
		ksnprintf(fatpath, n, "%s", rest);
		if (fatpath[0] == '\\') {
			fatpath[0] = '/';
		}
	} else {
		ksnprintf(fatpath, n, "/%s", rest);
	}
	return 1;
}

/** True when `abs` is the system-volume prefix (C: or /os). */
static int is_os_path(const char *abs)
{
	int d = 0;
	const char *rest = NULL;
	if (parse_drive(abs, &d, &rest) && d == DRV_C) {
		return 1;
	}
	return strcmp(abs, "/os") == 0 || str_starts(abs, "/os/");
}

/**
 * Select the FAT volume for `abs` and write the on-volume path to `fatpath`.
 * `C:` is the boot partition, `D:` leftover data, `E:` a second USB, `/os` = `C:`.
 */
static int map_vol(const char *abs, char *fatpath, size_t n)
{
	int drv = 0;
	if (split_abs(abs, &drv, fatpath, n)) {
		if (!drive_ready(drv)) {
			return 0;
		}
		fat_select(drive_vol(drv));
		return 1;
	}
	if (is_os_path(abs) || !fat_vol_ready(FAT_VOL_USR)) {
		fat_select(FAT_VOL_SYS);
		if (strcmp(abs, "/os") == 0) {
			ksnprintf(fatpath, n, "/");
		} else if (str_starts(abs, "/os/")) {
			ksnprintf(fatpath, n, "%s", abs + 3);
		} else {
			ksnprintf(fatpath, n, "%s", abs);
		}
		return 1;
	}
	fat_select(FAT_VOL_USR);
	ksnprintf(fatpath, n, "%s", abs);
	return 1;
}

/** Stat, including the synthetic `/os` directory on the data volume. */
static int fs_stat(const char *abs, struct fat_info *out)
{
	if (fat_vol_ready(FAT_VOL_USR) && (strcmp(abs, "/os") == 0)) {
		if (out) {
			ksnprintf(out->name, sizeof(out->name), "os");
			out->kind = FAT_DIR;
			out->size = 0;
			out->cluster = 0;
		}
		return 1;
	}
	char fp[FAT_PATH_MAX];
	if (!map_vol(abs, fp, sizeof(fp))) {
		return 0;
	}
	return fat_stat(fp, out) ? 1 : 0;
}

/** `/os` is reserved as the system mount point when a data volume exists. */
static int reserved_os(const char *abs)
{
	int drv = 0;
	char fp[FAT_PATH_MAX];
	if (split_abs(abs, &drv, fp, sizeof(fp)) && drv == DRV_C && strcmp(fp, "/") == 0) {
		return 1;
	}
	return fat_vol_ready(FAT_VOL_USR) && strcmp(abs, "/os") == 0;
}

static int read_mapped(const char *abs, void *buf, uint32_t cap, uint32_t *out_size);

void fs_init(void (*idle)(void))
{
	ready = 0;
	idle_fn = idle;
	cur_drive = DRV_D;
	for (int i = 0; i < DRV_N; i++) {
		ksnprintf(drive_cwd[i], sizeof(drive_cwd[i]), "/");
	}
	errbuf[0] = '\0';
	memset(&extra, 0, sizeof(extra));
	uint32_t phys = 0;
	file_buf_cap = 768u * 1024u;
	file_buf = phys_alloc(file_buf_cap, &phys);
	fat_set_idle(idle);
	if (!usb_msc_init(&disk, &extra, idle)) {
		ksnprintf(errbuf, sizeof(errbuf), "%s", "no USB mass storage");
		return;
	}
	if (!fat_mount(&disk)) {
		ksnprintf(errbuf, sizeof(errbuf), "%s", fat_last_error());
		tty_set_color(TTY_COL_ERR);
		tty_printf("fs: %s\n", errbuf);
		tty_set_color(TTY_COL_FG);
		return;
	}
	if (extra.sectors != 0) {
		(void)fat_mount_extra(&extra);
	}
	if (fat_vol_ready(FAT_VOL_USR)) {
		cur_drive = DRV_D;
	} else {
		cur_drive = DRV_C;
	}
	ready = 1;
}

bool fs_ready(void)
{
	return ready && fat_mounted();
}

const char *fs_error(void)
{
	if (errbuf[0]) {
		return errbuf;
	}
	return fat_last_error();
}

void fs_cmd_mount(void)
{
	if (fs_ready()) {
		tty_printf("C: %s FAT32 system '%s'\n", disk.name, fat_vol_name(FAT_VOL_SYS));
		if (fat_vol_ready(FAT_VOL_USR)) {
			tty_printf("D: %s FAT32 data '%s'\n", disk.name, fat_vol_name(FAT_VOL_USR));
		}
		if (extra.sectors == 0) {
			(void)usb_msc_scan_extra(&extra);
		}
		if (extra.sectors != 0 && !fat_vol_ready(FAT_VOL_EXT)) {
			(void)fat_mount_extra(&extra);
		}
		if (fat_vol_ready(FAT_VOL_EXT)) {
			tty_printf("E: %s FAT32 extra '%s'\n", extra.name, fat_vol_name(FAT_VOL_EXT));
		}
		return;
	}
	fs_init(idle_fn);
	if (!fs_ready()) {
		tty_set_color(TTY_COL_ERR);
		tty_printf("mount failed: %s (never formats the system partition)\n", fs_error());
		tty_set_color(TTY_COL_FG);
	}
}

void fs_cmd_pwd(void)
{
	if (!need_fs()) {
		return;
	}
	char shown[FAT_PATH_MAX];
	format_abs(cur_drive, drive_cwd[cur_drive], shown, sizeof(shown));
	tty_printf("%s\n", shown);
}

void fs_cmd_cd(int argc, char **argv)
{
	if (!need_fs()) {
		return;
	}
	const char *rel;
	int dummy = 0;
	const char *rest = NULL;
	if (argc > 1) {
		rel = argv[1];
	} else if (parse_drive(argv[0], &dummy, &rest)) {
		rel = argv[0];
	} else {
		rel = fat_vol_ready(FAT_VOL_USR) ? "D:/" : "C:/";
	}
	char path[FAT_PATH_MAX];
	if (!abs_path(rel, path, sizeof(path))) {
		tty_puts("cd: bad path\n");
		return;
	}
	int drv = 0;
	char fp[FAT_PATH_MAX];
	if (!split_abs(path, &drv, fp, sizeof(fp))) {
		tty_puts("cd: bad path\n");
		return;
	}
	if (!drive_ready(drv)) {
		tty_set_color(TTY_COL_ERR);
		tty_printf("cd: %c: not mounted (try mount)\n", (char)('C' + drv));
		tty_set_color(TTY_COL_FG);
		return;
	}
	struct fat_info inf;
	if (!fs_stat(path, &inf) || inf.kind != FAT_DIR) {
		tty_set_color(TTY_COL_ERR);
		tty_printf("cd: %s\n", fat_last_error());
		tty_set_color(TTY_COL_FG);
		return;
	}
	cur_drive = drv;
	ksnprintf(drive_cwd[drv], sizeof(drive_cwd[drv]), "%s", fp);
}

void fs_cmd_ls(int argc, char **argv)
{
	if (!need_fs()) {
		return;
	}
	char path[FAT_PATH_MAX];
	if (!abs_path((argc > 1) ? argv[1] : ".", path, sizeof(path))) {
		return;
	}
	unsigned n = 0;
	int drv = 0;
	char fp[FAT_PATH_MAX];
	if (split_abs(path, &drv, fp, sizeof(fp)) && drv == DRV_D && strcmp(fp, "/") == 0
		&& fat_vol_ready(FAT_VOL_SYS)) {
		tty_printf("  C:/\n");
		tty_printf("  os/\n");
		n += 2;
	}
	if (fat_vol_ready(FAT_VOL_USR) && (strcmp(path, "/os") == 0
		|| (split_abs(path, &drv, fp, sizeof(fp)) && drv == DRV_C && strcmp(fp, "/") == 0))) {
		char fp[FAT_PATH_MAX];
		map_vol(path, fp, sizeof(fp));
		struct fat_iter it;
		if (!fat_iter_begin(fp, &it)) {
			tty_set_color(TTY_COL_ERR);
			tty_printf("ls: %s\n", fat_last_error());
			tty_set_color(TTY_COL_FG);
			return;
		}
		struct fat_info inf;
		while (fat_iter_next(&it, &inf)) {
			if (inf.kind == FAT_DIR) {
				tty_printf("  %s/\n", inf.name);
			} else {
				tty_printf("  %s  %u\n", inf.name, inf.size);
			}
			n++;
		}
	} else {
		char fp[FAT_PATH_MAX];
		map_vol(path, fp, sizeof(fp));
		struct fat_iter it;
		if (!fat_iter_begin(fp, &it)) {
			tty_set_color(TTY_COL_ERR);
			tty_printf("ls: %s\n", fat_last_error());
			tty_set_color(TTY_COL_FG);
			return;
		}
		struct fat_info inf;
		while (fat_iter_next(&it, &inf)) {
			if (drv == DRV_D && strcmp(fp, "/") == 0
				&& (strcmp(inf.name, "os") == 0 || strcmp(inf.name, "OS") == 0)) {
				continue;
			}
			if (inf.kind == FAT_DIR) {
				tty_printf("  %s/\n", inf.name);
			} else {
				tty_printf("  %s  %u\n", inf.name, inf.size);
			}
			n++;
		}
	}
	if (n == 0) {
		tty_set_color(TTY_COL_DIM);
		tty_puts("  (empty)\n");
		tty_set_color(TTY_COL_FG);
	}
}

void fs_cmd_mkdir(int argc, char **argv)
{
	if (!need_fs()) {
		return;
	}
	if (argc < 2) {
		tty_puts("usage: mkdir <dir>\n");
		return;
	}
	char path[FAT_PATH_MAX];
	abs_path(argv[1], path, sizeof(path));
	if (reserved_os(path)) {
		tty_puts("mkdir: /os is the system volume\n");
		return;
	}
	char fp[FAT_PATH_MAX];
	map_vol(path, fp, sizeof(fp));
	if (!fat_mkdir(fp)) {
		tty_set_color(TTY_COL_ERR);
		tty_printf("mkdir: %s\n", fat_last_error());
		tty_set_color(TTY_COL_FG);
		return;
	}
	tty_printf("created %s\n", path);
}

void fs_cmd_rm(int argc, char **argv)
{
	if (!need_fs()) {
		return;
	}
	if (argc < 2) {
		tty_puts("usage: rm <path>\n");
		return;
	}
	char path[FAT_PATH_MAX];
	abs_path(argv[1], path, sizeof(path));
	if (reserved_os(path)) {
		tty_puts("rm: cannot remove /os\n");
		return;
	}
	char fp[FAT_PATH_MAX];
	map_vol(path, fp, sizeof(fp));
	if (!fat_remove(fp)) {
		tty_set_color(TTY_COL_ERR);
		tty_printf("rm: %s\n", fat_last_error());
		tty_set_color(TTY_COL_FG);
		return;
	}
	tty_printf("removed %s\n", path);
}

void fs_cmd_cp(int argc, char **argv)
{
	if (!need_fs()) {
		return;
	}
	if (argc < 3) {
		tty_puts("usage: cp <src> <dst>\n");
		return;
	}
	char src[FAT_PATH_MAX];
	char dst[FAT_PATH_MAX];
	abs_path(argv[1], src, sizeof(src));
	abs_path(argv[2], dst, sizeof(dst));
	if (reserved_os(dst)) {
		tty_puts("cp: cannot replace /os\n");
		return;
	}
	char sfp[FAT_PATH_MAX];
	char dfp[FAT_PATH_MAX];
	map_vol(src, sfp, sizeof(sfp));
	enum fat_vol_id src_vol = fat_current();
	map_vol(dst, dfp, sizeof(dfp));
	enum fat_vol_id dst_vol = fat_current();
	int ok = 0;
	if (src_vol == dst_vol) {
		fat_select(src_vol);
		ok = fat_copy(sfp, dfp) ? 1 : 0;
	} else if (file_buf != NULL) {
		uint32_t n = 0;
		fat_select(src_vol);
		if (fat_read(sfp, file_buf, file_buf_cap, &n)) {
			fat_select(dst_vol);
			ok = fat_write(dfp, file_buf, n) ? 1 : 0;
		}
	}
	if (!ok) {
		tty_set_color(TTY_COL_ERR);
		tty_printf("cp: %s\n", fat_last_error());
		tty_set_color(TTY_COL_FG);
		return;
	}
	tty_printf("copied to %s\n", dst);
}

void fs_cmd_mv(int argc, char **argv)
{
	if (!need_fs()) {
		return;
	}
	if (argc < 3) {
		tty_puts("usage: mv <src> <dst>\n");
		return;
	}
	char src[FAT_PATH_MAX];
	char dst[FAT_PATH_MAX];
	abs_path(argv[1], src, sizeof(src));
	abs_path(argv[2], dst, sizeof(dst));
	if (reserved_os(src) || reserved_os(dst)) {
		tty_puts("mv: cannot move /os\n");
		return;
	}
	char sfp[FAT_PATH_MAX];
	char dfp[FAT_PATH_MAX];
	map_vol(src, sfp, sizeof(sfp));
	enum fat_vol_id src_vol = fat_current();
	map_vol(dst, dfp, sizeof(dfp));
	enum fat_vol_id dst_vol = fat_current();
	if (src_vol != dst_vol) {
		tty_puts("mv: use cp to copy between /os and the data volume\n");
		return;
	}
	fat_select(src_vol);
	if (!fat_rename(sfp, dfp)) {
		tty_set_color(TTY_COL_ERR);
		tty_printf("mv: %s\n", fat_last_error());
		tty_set_color(TTY_COL_FG);
		return;
	}
	tty_printf("moved to %s\n", dst);
}

void fs_cmd_type(int argc, char **argv)
{
	if (!need_fs()) {
		return;
	}
	if (argc < 2) {
		tty_puts("usage: type <file>\n");
		return;
	}
	char path[FAT_PATH_MAX];
	abs_path(argv[1], path, sizeof(path));
	uint32_t n = 0;
	if (file_buf == NULL || !read_mapped(path, file_buf, file_buf_cap, &n)) {
		tty_set_color(TTY_COL_ERR);
		tty_printf("type: %s\n", fat_last_error());
		tty_set_color(TTY_COL_FG);
		return;
	}
	uint32_t show = n;
	if (show > 4096) {
		show = 4096;
	}
	for (uint32_t i = 0; i < show; i++) {
		char c = (char)file_buf[i];
		if (c == '\n' || (c >= 32 && c < 127) || c == '\t') {
			tty_putc(c);
		} else {
			tty_putc('.');
		}
	}
	if (n && file_buf[show - 1] != '\n') {
		tty_putc('\n');
	}
	if (n > show) {
		tty_printf("... %u bytes total\n", n);
	}
}

void fs_cmd_touch(int argc, char **argv)
{
	if (!need_fs()) {
		return;
	}
	if (argc < 2) {
		tty_puts("usage: touch <file>\n");
		return;
	}
	char path[FAT_PATH_MAX];
	abs_path(argv[1], path, sizeof(path));
	if (reserved_os(path)) {
		tty_puts("touch: /os is the system volume\n");
		return;
	}
	char fp[FAT_PATH_MAX];
	map_vol(path, fp, sizeof(fp));
	if (!fat_touch(fp)) {
		tty_set_color(TTY_COL_ERR);
		tty_printf("touch: %s\n", fat_last_error());
		tty_set_color(TTY_COL_FG);
		return;
	}
	tty_printf("touched %s\n", path);
}

void fs_cmd_info(int argc, char **argv)
{
	if (!need_fs()) {
		return;
	}
	if (argc < 2) {
		tty_puts("usage: info <path>\n");
		return;
	}
	char path[FAT_PATH_MAX];
	abs_path(argv[1], path, sizeof(path));
	struct fat_info inf;
	if (!fs_stat(path, &inf)) {
		tty_set_color(TTY_COL_ERR);
		tty_printf("info: %s\n", fat_last_error());
		tty_set_color(TTY_COL_FG);
		return;
	}
	tty_puts("File info\n");
	tty_set_color(TTY_COL_DIM);
	tty_printf("path:     %s\n", path);
	tty_printf("name:     %s\n", inf.name);
	tty_printf("type:     %s\n", inf.kind == FAT_DIR ? "directory" : "file");
	tty_printf("size:     %u bytes\n", inf.size);
	tty_printf("cluster:  %u\n", inf.cluster);
	tty_set_color(TTY_COL_FG);
}

void fs_cmd_storage(void)
{
	if (!need_fs()) {
		return;
	}
	tty_puts("Storage\n");
	tty_set_color(TTY_COL_DIM);
	tty_printf("device:   %s\n", disk.name);
	tty_printf("disk:     %llu bytes\n", (unsigned long long)disk.sectors * 512ull);
	tty_printf("C:        FAT32 '%s' (system)\n", fat_vol_name(FAT_VOL_SYS));
	tty_printf("  total:  %llu bytes\n", (unsigned long long)fat_vol_bytes_total(FAT_VOL_SYS));
	tty_printf("  free:   %llu bytes\n", (unsigned long long)fat_vol_bytes_free(FAT_VOL_SYS));
	if (fat_vol_ready(FAT_VOL_USR)) {
		tty_printf("D:        FAT32 '%s' (data)\n", fat_vol_name(FAT_VOL_USR));
		tty_printf("  total:  %llu bytes\n", (unsigned long long)fat_vol_bytes_total(FAT_VOL_USR));
		tty_printf("  free:   %llu bytes\n", (unsigned long long)fat_vol_bytes_free(FAT_VOL_USR));
	}
	if (fat_vol_ready(FAT_VOL_EXT)) {
		tty_printf("E:        FAT32 '%s' (extra USB)\n", fat_vol_name(FAT_VOL_EXT));
		tty_printf("  total:  %llu bytes\n", (unsigned long long)fat_vol_bytes_total(FAT_VOL_EXT));
		tty_printf("  free:   %llu bytes\n", (unsigned long long)fat_vol_bytes_free(FAT_VOL_EXT));
	}
	tty_set_color(TTY_COL_FG);
}

/** Read a file, falling back to `/os/...` so stock WAVs still resolve. */
static int read_mapped(const char *abs, void *buf, uint32_t cap, uint32_t *out_size)
{
	char fp[FAT_PATH_MAX];
	if (!map_vol(abs, fp, sizeof(fp))) {
		return 0;
	}
	if (fat_read(fp, buf, cap, out_size)) {
		return 1;
	}
	/*
	 * Names typed on D: still resolve stock files on C:
	 * (`load audio/test.wav` while cwd is D:/).
	 */
	int drv = 0;
	char fatp[FAT_PATH_MAX];
	if (split_abs(abs, &drv, fatp, sizeof(fatp)) && drv == DRV_D
		&& strcmp(fatp, "/") != 0 && fat_vol_ready(FAT_VOL_SYS)) {
		char alt[FAT_PATH_MAX];
		format_abs(DRV_C, fatp, alt, sizeof(alt));
		if (map_vol(alt, fp, sizeof(fp)) && fat_read(fp, buf, cap, out_size)) {
			return 1;
		}
	}
	return 0;
}

bool fs_read_file(const char *path, void *buf, uint32_t cap, uint32_t *out_size)
{
	if (!fs_ready()) {
		return false;
	}
	char abs[FAT_PATH_MAX];
	if (!abs_path(path, abs, sizeof(abs))) {
		return false;
	}
	return read_mapped(abs, buf, cap, out_size) ? true : false;
}

bool fs_write_file(const char *path, const void *buf, uint32_t size)
{
	if (!fs_ready()) {
		ksnprintf(errbuf, sizeof(errbuf), "%s", "no filesystem");
		return false;
	}
	char abs[FAT_PATH_MAX];
	if (!abs_path(path, abs, sizeof(abs))) {
		ksnprintf(errbuf, sizeof(errbuf), "%s", "bad path");
		return false;
	}
	if (reserved_os(abs)) {
		ksnprintf(errbuf, sizeof(errbuf), "%s", "/os is the system volume");
		return false;
	}
	char fp[FAT_PATH_MAX];
	map_vol(abs, fp, sizeof(fp));
	if (!fat_write(fp, buf, size)) {
		ksnprintf(errbuf, sizeof(errbuf), "%s", fat_last_error());
		return false;
	}
	return true;
}

/** Copy one file across volumes using the shared I/O buffer. */
static int copy_abs(const char *src, const char *dst)
{
	char sfp[FAT_PATH_MAX];
	char dfp[FAT_PATH_MAX];
	if (!map_vol(src, sfp, sizeof(sfp))) {
		return 0;
	}
	enum fat_vol_id src_vol = fat_current();
	if (!map_vol(dst, dfp, sizeof(dfp))) {
		return 0;
	}
	enum fat_vol_id dst_vol = fat_current();
	if (src_vol == dst_vol) {
		fat_select(src_vol);
		return fat_copy(sfp, dfp) ? 1 : 0;
	}
	if (file_buf == NULL) {
		return 0;
	}
	uint32_t n = 0;
	fat_select(src_vol);
	if (!fat_read(sfp, file_buf, file_buf_cap, &n)) {
		return 0;
	}
	fat_select(dst_vol);
	return fat_write(dfp, file_buf, n) ? 1 : 0;
}

void fs_cmd_drives(void)
{
	if (!need_fs()) {
		return;
	}
	fs_cmd_mount();
	char shown[FAT_PATH_MAX];
	format_abs(cur_drive, drive_cwd[cur_drive], shown, sizeof(shown));
	tty_printf("cwd      %s\n", shown);
	tty_set_color(TTY_COL_DIM);
	tty_puts("cd C: / cd D: / cd E:  switch volume.  /os is C:\n");
	tty_puts("plug another USB and type mount to bind it as E:\n");
	tty_set_color(TTY_COL_FG);
}

void fs_cmd_update(int argc, char **argv)
{
	if (!need_fs()) {
		return;
	}
	/* A stick plugged after boot can hold the new kernel as E:. */
	if (extra.sectors == 0) {
		(void)usb_msc_scan_extra(&extra);
	}
	if (extra.sectors != 0 && !fat_vol_ready(FAT_VOL_EXT)) {
		(void)fat_mount_extra(&extra);
	}
	char src[FAT_PATH_MAX];
	src[0] = '\0';
	if (argc > 1) {
		abs_path(argv[1], src, sizeof(src));
		struct fat_info inf;
		if (fs_stat(src, &inf) && inf.kind == FAT_DIR) {
			char dir[FAT_PATH_MAX];
			ksnprintf(dir, sizeof(dir), "%s", src);
			ksnprintf(src, sizeof(src), "%s/boot/kernel", dir);
			if (!fs_stat(src, &inf) || inf.kind != FAT_FILE) {
				ksnprintf(src, sizeof(src), "%s/kernel", dir);
			}
		}
	} else {
		static const char *cands[] = {
			"E:/boot/kernel",
			"D:/update/boot/kernel",
			"D:/boot/kernel",
			NULL
		};
		for (int i = 0; cands[i]; i++) {
			struct fat_info inf;
			if (fs_stat(cands[i], &inf) && inf.kind == FAT_FILE) {
				ksnprintf(src, sizeof(src), "%s", cands[i]);
				break;
			}
		}
	}
	if (src[0] == '\0') {
		tty_puts("usage: update [kernel|dir]\n");
		tty_puts("copy a new kernel onto C: without touching D:\n");
		tty_puts("looks for E:/boot/kernel, D:/update/boot/kernel, D:/boot/kernel\n");
		tty_puts("host: tools/update-system.ps1  or  python3 tools/update_system.py\n");
		return;
	}
	struct fat_info inf;
	if (!fs_stat(src, &inf) || inf.kind != FAT_FILE) {
		tty_set_color(TTY_COL_ERR);
		tty_printf("update: no kernel at %s\n", src);
		tty_set_color(TTY_COL_FG);
		return;
	}
	if (!copy_abs(src, "C:/boot/kernel")) {
		tty_set_color(TTY_COL_ERR);
		tty_printf("update: copy failed: %s\n", fat_last_error());
		tty_set_color(TTY_COL_FG);
		return;
	}
	tty_printf("updated C:/boot/kernel from %s (%u bytes)\n", src, inf.size);
	/* Same folder may have a new limine.conf — optional. */
	char conf[FAT_PATH_MAX];
	ksnprintf(conf, sizeof(conf), "%s", src);
	char *slash = conf;
	char *last = conf;
	while (*slash) {
		if (*slash == '/') {
			last = slash;
		}
		slash++;
	}
	ksnprintf(last, sizeof(conf) - (size_t)(last - conf), "/limine.conf");
	if (fs_stat(conf, &inf) && inf.kind == FAT_FILE) {
		if (copy_abs(conf, "C:/boot/limine/limine.conf")) {
			tty_puts("updated C:/boot/limine/limine.conf\n");
		}
	}
	tty_set_color(TTY_COL_AUDIO);
	tty_puts("D: was not touched. reboot to run the new kernel.\n");
	tty_set_color(TTY_COL_FG);
}

uint8_t *fs_iobuf(uint32_t *cap)
{
	if (cap) {
		*cap = file_buf_cap;
	}
	return file_buf;
}

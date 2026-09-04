#include "fs.h"
#include "fat.h"
#include "klib.h"
#include "phys.h"
#include "tty.h"
#include "usb.h"

#include <stddef.h>

static struct blkdev disk;
static int ready;
static char cwd[FAT_PATH_MAX];
static char errbuf[80];
static uint8_t *file_buf;
static uint32_t file_buf_cap;
static void (*idle_fn)(void);

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

/** Build an absolute path from `rel` into `out`. */
static int abs_path(const char *rel, char *out, size_t n)
{
	char raw[FAT_PATH_MAX];
	if (rel == NULL || rel[0] == '\0') {
		ksnprintf(out, n, "%s", cwd);
		return 1;
	}
	if (rel[0] == '/') {
		ksnprintf(raw, sizeof(raw), "%s", rel);
	} else if (strcmp(cwd, "/") == 0) {
		ksnprintf(raw, sizeof(raw), "/%s", rel);
	} else {
		ksnprintf(raw, sizeof(raw), "%s/%s", cwd, rel);
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
		ksnprintf(out, n, "/");
		return 1;
	}
	size_t off = 0;
	out[0] = '\0';
	for (unsigned i = 0; i < np; i++) {
		size_t used = strlen(out);
		ksnprintf(out + used, n - used, "/%s", parts[i]);
		(void)off;
	}
	return 1;
}

/** True when `abs` is the system-volume prefix. */
static int is_os_path(const char *abs)
{
	return strcmp(abs, "/os") == 0 || str_starts(abs, "/os/");
}

/**
 * Select the FAT volume for `abs` and write the on-volume path to `fatpath`.
 * `/os` and `/os/...` always address the boot/system partition.
 */
static int map_vol(const char *abs, char *fatpath, size_t n)
{
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
	if (fat_vol_ready(FAT_VOL_USR) && strcmp(abs, "/os") == 0) {
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
	return fat_vol_ready(FAT_VOL_USR) && strcmp(abs, "/os") == 0;
}

static int read_mapped(const char *abs, void *buf, uint32_t cap, uint32_t *out_size);

void fs_init(void (*idle)(void))
{
	ready = 0;
	idle_fn = idle;
	ksnprintf(cwd, sizeof(cwd), "/");
	errbuf[0] = '\0';
	uint32_t phys = 0;
	file_buf_cap = 768u * 1024u;
	file_buf = phys_alloc(file_buf_cap, &phys);
	fat_set_idle(idle);
	if (!usb_msc_init(&disk, idle)) {
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
		tty_printf("mounted %s FAT32 system '%s'\n", disk.name, fat_vol_name(FAT_VOL_SYS));
		if (fat_vol_ready(FAT_VOL_USR)) {
			tty_printf("mounted %s FAT32 data '%s'\n", disk.name, fat_vol_name(FAT_VOL_USR));
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
	tty_printf("%s\n", cwd);
}

void fs_cmd_cd(int argc, char **argv)
{
	if (!need_fs()) {
		return;
	}
	const char *rel = (argc > 1) ? argv[1] : "/";
	char path[FAT_PATH_MAX];
	if (!abs_path(rel, path, sizeof(path))) {
		tty_puts("cd: bad path\n");
		return;
	}
	struct fat_info inf;
	if (!fs_stat(path, &inf) || inf.kind != FAT_DIR) {
		tty_set_color(TTY_COL_ERR);
		tty_printf("cd: %s\n", fat_last_error());
		tty_set_color(TTY_COL_FG);
		return;
	}
	ksnprintf(cwd, sizeof(cwd), "%s", path);
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
	if (fat_vol_ready(FAT_VOL_USR) && strcmp(path, "/") == 0) {
		tty_printf("  os/\n");
		n++;
	}
	if (fat_vol_ready(FAT_VOL_USR) && strcmp(path, "/os") == 0) {
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
			if (fat_vol_ready(FAT_VOL_USR) && strcmp(path, "/") == 0
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

void fs_cmd_cat(int argc, char **argv)
{
	if (!need_fs()) {
		return;
	}
	if (argc < 2) {
		tty_puts("usage: cat <file>\n");
		return;
	}
	char path[FAT_PATH_MAX];
	abs_path(argv[1], path, sizeof(path));
	uint32_t n = 0;
	if (file_buf == NULL || !read_mapped(path, file_buf, file_buf_cap, &n)) {
		tty_set_color(TTY_COL_ERR);
		tty_printf("cat: %s\n", fat_last_error());
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
	tty_printf("system:   FAT32 '%s'\n", fat_vol_name(FAT_VOL_SYS));
	tty_printf("  total:  %llu bytes\n", (unsigned long long)fat_vol_bytes_total(FAT_VOL_SYS));
	tty_printf("  free:   %llu bytes\n", (unsigned long long)fat_vol_bytes_free(FAT_VOL_SYS));
	if (fat_vol_ready(FAT_VOL_USR)) {
		tty_printf("data:     FAT32 '%s'\n", fat_vol_name(FAT_VOL_USR));
		tty_printf("data total: %llu\n", (unsigned long long)fat_vol_bytes_total(FAT_VOL_USR));
		tty_printf("  free:   %llu bytes\n", (unsigned long long)fat_vol_bytes_free(FAT_VOL_USR));
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
	if (fat_vol_ready(FAT_VOL_USR) && !is_os_path(abs) && strcmp(abs, "/") != 0) {
		char alt[FAT_PATH_MAX];
		ksnprintf(alt, sizeof(alt), "/os%s", abs);
		map_vol(alt, fp, sizeof(fp));
		return fat_read(fp, buf, cap, out_size) ? 1 : 0;
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

uint8_t *fs_iobuf(uint32_t *cap)
{
	if (cap) {
		*cap = file_buf_cap;
	}
	return file_buf;
}

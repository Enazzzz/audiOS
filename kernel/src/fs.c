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

void fs_init(void (*idle)(void))
{
	ready = 0;
	idle_fn = idle;
	ksnprintf(cwd, sizeof(cwd), "/");
	errbuf[0] = '\0';
	uint32_t phys = 0;
	file_buf_cap = 768u * 1024u;
	file_buf = phys_alloc(file_buf_cap, &phys);
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
	tty_set_color(TTY_COL_AUDIO);
	tty_printf("fs: mounted FAT32 '%s'\n", fat_volume());
	tty_set_color(TTY_COL_FG);
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
		tty_printf("mounted %s FAT32 '%s'\n", disk.name, fat_volume());
		return;
	}
	fs_init(idle_fn);
	if (!fs_ready()) {
		tty_set_color(TTY_COL_ERR);
		tty_printf("mount failed: %s (never formats the disk)\n", fs_error());
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
	if (!fat_stat(path, &inf) || inf.kind != FAT_DIR) {
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
	struct fat_iter it;
	if (!fat_iter_begin(path, &it)) {
		tty_set_color(TTY_COL_ERR);
		tty_printf("ls: %s\n", fat_last_error());
		tty_set_color(TTY_COL_FG);
		return;
	}
	struct fat_info inf;
	unsigned n = 0;
	while (fat_iter_next(&it, &inf)) {
		if (inf.kind == FAT_DIR) {
			tty_printf("  %s/\n", inf.name);
		} else {
			tty_printf("  %s  %u\n", inf.name, inf.size);
		}
		n++;
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
	if (!fat_mkdir(path)) {
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
	if (!fat_remove(path)) {
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
	if (!fat_copy(src, dst)) {
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
	if (!fat_rename(src, dst)) {
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
	if (file_buf == NULL || !fat_read(path, file_buf, file_buf_cap, &n)) {
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
	if (!fat_touch(path)) {
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
	if (!fat_stat(path, &inf)) {
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
	uint64_t tot = fat_bytes_total();
	uint64_t fr = fat_bytes_free();
	tty_puts("Storage\n");
	tty_set_color(TTY_COL_DIM);
	tty_printf("device:   %s\n", disk.name);
	tty_printf("fs:       FAT32 '%s'\n", fat_volume());
	tty_printf("total:    %llu bytes\n", (unsigned long long)tot);
	tty_printf("free:     %llu bytes\n", (unsigned long long)fr);
	tty_printf("used:     %llu bytes\n", (unsigned long long)(tot - fr));
	tty_set_color(TTY_COL_FG);
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
	return fat_read(abs, buf, cap, out_size);
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
	if (!fat_write(abs, buf, size)) {
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

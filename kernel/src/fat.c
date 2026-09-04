#include "fat.h"
#include "klib.h"
#include "phys.h"
#include "tty.h"

#include <stddef.h>

#define SECTOR		512u
#define EOC		0x0FFFFFF8u
#define BAD		0x0FFFFFF7u
#define ATTR_RO		0x01
#define ATTR_HID	0x02
#define ATTR_SYS	0x04
#define ATTR_VOL	0x08
#define ATTR_DIR	0x10
#define ATTR_ARC	0x20
#define ATTR_LFN	0x0F

/* Per-volume FAT32 state. Short macros keep the existing helpers readable. */
struct fat_vol {
	struct blkdev *f_bd;
	uint32_t f_part_lba;
	uint32_t f_fat_sz;
	uint32_t f_fat_begin;
	uint32_t f_data_begin;
	uint32_t f_root_clus;
	uint32_t f_spc;
	uint32_t f_cluster_bytes;
	uint32_t f_total_clusters;
	uint32_t f_fat_cache_sec;
	uint8_t f_fat_cache[SECTOR];
	int f_fat_cache_dirty;
	uint8_t *f_clus_buf;
	char f_volume[12];
	int f_mounted;
};

static struct fat_vol vols[2];
static struct fat_vol *v;
static void (*idle_cb)(void);
static char last_err[80];
static enum fat_vol_id current_vol;

#define bd		(v->f_bd)
#define part_lba	(v->f_part_lba)
#define fat_sz		(v->f_fat_sz)
#define fat_begin	(v->f_fat_begin)
#define data_begin	(v->f_data_begin)
#define root_clus	(v->f_root_clus)
#define spc		(v->f_spc)
#define cluster_bytes	(v->f_cluster_bytes)
#define total_clusters	(v->f_total_clusters)
#define fat_cache_sec	(v->f_fat_cache_sec)
#define fat_cache	(v->f_fat_cache)
#define fat_cache_dirty	(v->f_fat_cache_dirty)
#define clus_buf	(v->f_clus_buf)
#define volume		(v->f_volume)
#define mounted		(v->f_mounted)

/** Call the audio idle hook so USB mkfs does not underrun the DAC. */
static void fat_pump(void)
{
	if (idle_cb) {
		idle_cb();
	}
}

void fat_set_idle(void (*idle)(void))
{
	idle_cb = idle;
}

/** Record a non-fatal filesystem error. */
static void fat_fail(const char *msg)
{
	ksnprintf(last_err, sizeof(last_err), "%s", msg);
}

const char *fat_last_error(void)
{
	return last_err[0] ? last_err : "ok";
}

static int fat_flush(void);

bool fat_mounted(void)
{
	return vols[FAT_VOL_SYS].f_mounted != 0;
}

bool fat_vol_ready(enum fat_vol_id id)
{
	switch (id) {
	case FAT_VOL_SYS:
	case FAT_VOL_USR:
		return vols[id].f_mounted != 0;
	default: {
		enum fat_vol_id unknown = id;
		(void)unknown;
		return false;
	}
	}
}

void fat_select(enum fat_vol_id id)
{
	switch (id) {
	case FAT_VOL_SYS:
	case FAT_VOL_USR:
		break;
	default: {
		enum fat_vol_id unknown = id;
		(void)unknown;
		return;
	}
	}
	if (v != NULL && v->f_fat_cache_dirty) {
		(void)fat_flush();
	}
	v = &vols[id];
	current_vol = id;
}

enum fat_vol_id fat_current(void)
{
	return current_vol;
}

const char *fat_volume(void)
{
	return volume[0] ? volume : "UNNAMED";
}

/** Read `count` sectors at partition-relative LBA `lba`. */
static int rd(uint32_t lba, uint32_t count, void *buf)
{
	if (bd == NULL || bd->read == NULL) {
		return -1;
	}
	return bd->read(part_lba + lba, count, buf);
}

/** Write `count` sectors at partition-relative LBA `lba`. */
static int wr(uint32_t lba, uint32_t count, const void *buf)
{
	if (bd == NULL || bd->write == NULL) {
		return -1;
	}
	return bd->write(part_lba + lba, count, buf);
}

static uint32_t r16(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8);
}

static uint32_t r32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
		| ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void w16(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
}

static void w32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

/** Flush a dirty cached FAT sector to both FAT copies. */
static int fat_flush(void)
{
	if (!fat_cache_dirty || fat_cache_sec == 0xFFFFFFFFu) {
		return 0;
	}
	if (wr(fat_begin + fat_cache_sec, 1, fat_cache) != 0) {
		return -1;
	}
	if (wr(fat_begin + fat_sz + fat_cache_sec, 1, fat_cache) != 0) {
		return -1;
	}
	fat_cache_dirty = 0;
	return 0;
}

/** Bring FAT sector `sec` into the cache. */
static int fat_load(uint32_t sec)
{
	if (sec == fat_cache_sec) {
		return 0;
	}
	if (fat_flush() != 0) {
		return -1;
	}
	if (rd(fat_begin + sec, 1, fat_cache) != 0) {
		return -1;
	}
	fat_cache_sec = sec;
	return 0;
}

static uint32_t fat_get(uint32_t clus)
{
	uint32_t off = clus * 4u;
	if (fat_load(off / SECTOR) != 0) {
		return BAD;
	}
	return r32(fat_cache + (off % SECTOR)) & 0x0FFFFFFFu;
}

static int fat_set(uint32_t clus, uint32_t val)
{
	uint32_t off = clus * 4u;
	if (fat_load(off / SECTOR) != 0) {
		return -1;
	}
	uint32_t cur = r32(fat_cache + (off % SECTOR));
	w32(fat_cache + (off % SECTOR), (cur & 0xF0000000u) | (val & 0x0FFFFFFFu));
	fat_cache_dirty = 1;
	return 0;
}

/** First sector of cluster `clus`. */
static uint32_t clus_lba(uint32_t clus)
{
	return data_begin + (clus - 2u) * spc;
}

static int read_clus(uint32_t clus)
{
	if (clus < 2 || clus >= EOC) {
		return -1;
	}
	return rd(clus_lba(clus), spc, clus_buf);
}

static int write_clus(uint32_t clus)
{
	if (clus < 2 || clus >= EOC) {
		return -1;
	}
	return wr(clus_lba(clus), spc, clus_buf);
}

static uint32_t alloc_clus(void)
{
	for (uint32_t c = 2; c < total_clusters + 2; c++) {
		if (fat_get(c) == 0) {
			if (fat_set(c, 0x0FFFFFFFu) != 0) {
				return 0;
			}
			memset(clus_buf, 0, cluster_bytes);
			if (write_clus(c) != 0) {
				return 0;
			}
			if (fat_flush() != 0) {
				return 0;
			}
			return c;
		}
	}
	fat_fail("disk full");
	return 0;
}

static int free_chain(uint32_t clus)
{
	while (clus >= 2 && clus < EOC) {
		uint32_t next = fat_get(clus);
		if (fat_set(clus, 0) != 0) {
			return -1;
		}
		clus = next;
	}
	return fat_flush();
}

static int ascii_eq(char a, char b)
{
	if (a >= 'A' && a <= 'Z') {
		a = (char)(a - 'A' + 'a');
	}
	if (b >= 'A' && b <= 'Z') {
		b = (char)(b - 'A' + 'a');
	}
	return a == b;
}

static int name_eq(const char *a, const char *b)
{
	while (*a && *b) {
		if (!ascii_eq(*a, *b)) {
			return 0;
		}
		a++;
		b++;
	}
	return *a == '\0' && *b == '\0';
}

/** Decode an 8.3 entry into a dotted name. */
static void decode83(const uint8_t *ent, char *out, size_t n)
{
	char stem[9];
	char ext[4];
	unsigned s = 0;
	unsigned e = 0;
	for (unsigned i = 0; i < 8; i++) {
		if (ent[i] != ' ') {
			stem[s++] = (char)ent[i];
		}
	}
	stem[s] = '\0';
	for (unsigned i = 8; i < 11; i++) {
		if (ent[i] != ' ') {
			ext[e++] = (char)ent[i];
		}
	}
	ext[e] = '\0';
	if (e) {
		ksnprintf(out, n, "%s.%s", stem, ext);
	} else {
		ksnprintf(out, n, "%s", stem);
	}
}

/** Build a padded 8.3 name. Returns 0 if the name cannot fit. */
static int encode83(const char *name, uint8_t out[11])
{
	memset(out, ' ', 11);
	if (strcmp(name, ".") == 0) {
		out[0] = '.';
		return 1;
	}
	if (strcmp(name, "..") == 0) {
		out[0] = '.';
		out[1] = '.';
		return 1;
	}
	char tmp[12];
	unsigned n = 0;
	const char *dot = NULL;
	for (const char *p = name; *p; p++) {
		if (*p == '.') {
			dot = p;
		}
	}
	const char *end = dot ? dot : name + strlen(name);
	for (const char *p = name; p < end && n < 8; p++) {
		char c = *p;
		if (c >= 'a' && c <= 'z') {
			c = (char)(c - 'a' + 'A');
		}
		if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) {
			c = '_';
		}
		tmp[n++] = c;
	}
	memcpy(out, tmp, n);
	if (dot && dot[1]) {
		unsigned k = 0;
		for (const char *p = dot + 1; *p && k < 3; p++) {
			char c = *p;
			if (c >= 'a' && c <= 'z') {
				c = (char)(c - 'a' + 'A');
			}
			if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) {
				c = '_';
			}
			out[8 + k] = (uint8_t)c;
			k++;
		}
	}
	return 1;
}

/** Append one LFN UTF-16 character as ASCII if possible. */
static void lfn_char(char *dst, unsigned *n, unsigned max, uint8_t lo, uint8_t hi)
{
	if (*n + 1 >= max) {
		return;
	}
	if (lo == 0xFF && hi == 0xFF) {
		return;
	}
	if (lo == 0 && hi == 0) {
		return;
	}
	dst[(*n)++] = (hi == 0 && lo >= 32 && lo < 127) ? (char)lo : '?';
}

/** Decode concatenated LFN entries (already in order, last first in `parts`). */
static void decode_lfn(uint8_t parts[][32], unsigned np, char *out, size_t n)
{
	out[0] = '\0';
	unsigned len = 0;
	for (int i = (int)np - 1; i >= 0; i--) {
		const uint8_t *e = parts[i];
		lfn_char(out, &len, (unsigned)n, e[1], e[2]);
		lfn_char(out, &len, (unsigned)n, e[3], e[4]);
		lfn_char(out, &len, (unsigned)n, e[5], e[6]);
		lfn_char(out, &len, (unsigned)n, e[7], e[8]);
		lfn_char(out, &len, (unsigned)n, e[9], e[10]);
		lfn_char(out, &len, (unsigned)n, e[14], e[15]);
		lfn_char(out, &len, (unsigned)n, e[16], e[17]);
		lfn_char(out, &len, (unsigned)n, e[18], e[19]);
		lfn_char(out, &len, (unsigned)n, e[20], e[21]);
		lfn_char(out, &len, (unsigned)n, e[22], e[23]);
		lfn_char(out, &len, (unsigned)n, e[24], e[25]);
		lfn_char(out, &len, (unsigned)n, e[28], e[29]);
		lfn_char(out, &len, (unsigned)n, e[30], e[31]);
	}
	out[len] = '\0';
}

static uint32_t ent_clus(const uint8_t *ent)
{
	return ((uint32_t)r16(ent + 20) << 16) | r16(ent + 26);
}

static void ent_set_clus(uint8_t *ent, uint32_t clus)
{
	w16(ent + 20, clus >> 16);
	w16(ent + 26, clus & 0xFFFF);
}

struct dir_walk {
	uint32_t clus;
	uint32_t index;
};

/** Read directory entry `index` (32-byte slots) of directory `clus`. */
static int dir_get(uint32_t clus, uint32_t index, uint8_t ent[32])
{
	uint32_t per = cluster_bytes / 32u;
	uint32_t skip = index / per;
	uint32_t c = clus;
	for (uint32_t i = 0; i < skip; i++) {
		c = fat_get(c);
		if (c < 2 || c >= EOC) {
			return 0;
		}
	}
	if (read_clus(c) != 0) {
		return -1;
	}
	memcpy(ent, clus_buf + (index % per) * 32u, 32);
	return 1;
}

/** Write directory entry `index` of directory `clus`. */
static int dir_put(uint32_t clus, uint32_t index, const uint8_t ent[32])
{
	uint32_t per = cluster_bytes / 32u;
	uint32_t skip = index / per;
	uint32_t c = clus;
	for (uint32_t i = 0; i < skip; i++) {
		c = fat_get(c);
		if (c < 2 || c >= EOC) {
			return -1;
		}
	}
	if (read_clus(c) != 0) {
		return -1;
	}
	memcpy(clus_buf + (index % per) * 32u, ent, 32);
	return write_clus(c);
}

/** Find `name` in directory `dir_clus`. Fills `info` and optional slot index. */
static int dir_find(uint32_t dir_clus, const char *name, struct fat_info *info, uint32_t *slot)
{
	uint8_t lfn[8][32];
	unsigned nlfn = 0;
	for (uint32_t i = 0; i < 4096; i++) {
		uint8_t ent[32];
		int r = dir_get(dir_clus, i, ent);
		if (r <= 0) {
			return 0;
		}
		if (ent[0] == 0x00) {
			return 0;
		}
		if (ent[0] == 0xE5) {
			nlfn = 0;
			continue;
		}
		if (ent[11] == ATTR_LFN) {
			if (nlfn < 8) {
				memcpy(lfn[nlfn++], ent, 32);
			}
			continue;
		}
		if (ent[11] & ATTR_VOL) {
			nlfn = 0;
			continue;
		}
		char shortn[16];
		char longn[FAT_NAME_MAX];
		decode83(ent, shortn, sizeof(shortn));
		longn[0] = '\0';
		if (nlfn) {
			decode_lfn(lfn, nlfn, longn, sizeof(longn));
		}
		nlfn = 0;
		if (!name_eq(shortn, name) && (longn[0] == '\0' || !name_eq(longn, name))) {
			continue;
		}
		if (info) {
			ksnprintf(info->name, sizeof(info->name), "%s", longn[0] ? longn : shortn);
			info->kind = (ent[11] & ATTR_DIR) ? FAT_DIR : FAT_FILE;
			info->size = r32(ent + 28);
			info->cluster = ent_clus(ent);
		}
		if (slot) {
			*slot = i;
		}
		return 1;
	}
	return 0;
}

/** Split `path` into parent dir cluster and final component. */
static int walk_parent(const char *path, uint32_t *parent, char *leaf, size_t leafn)
{
	if (path == NULL || path[0] != '/') {
		fat_fail("path must be absolute");
		return 0;
	}
	uint32_t clus = root_clus;
	char part[FAT_NAME_MAX];
	const char *p = path;
	const char *last = path;
	while (*p == '/') {
		p++;
	}
	if (*p == '\0') {
		*parent = clus;
		ksnprintf(leaf, leafn, "%s", "");
		return 1;
	}
	for (;;) {
		unsigned n = 0;
		while (*p && *p != '/') {
			if (n + 1 < sizeof(part)) {
				part[n++] = *p;
			}
			p++;
		}
		part[n] = '\0';
		while (*p == '/') {
			p++;
		}
		if (*p == '\0') {
			*parent = clus;
			ksnprintf(leaf, leafn, "%s", part);
			return 1;
		}
		struct fat_info inf;
		if (!dir_find(clus, part, &inf, NULL) || inf.kind != FAT_DIR) {
			fat_fail("path not found");
			return 0;
		}
		clus = inf.cluster ? inf.cluster : root_clus;
		(void)last;
	}
}

bool fat_stat(const char *path, struct fat_info *out)
{
	if (!mounted) {
		fat_fail("not mounted");
		return false;
	}
	if (path == NULL || (path[0] == '/' && path[1] == '\0')) {
		if (out) {
			ksnprintf(out->name, sizeof(out->name), "/");
			out->kind = FAT_DIR;
			out->size = 0;
			out->cluster = root_clus;
		}
		return true;
	}
	uint32_t parent;
	char leaf[FAT_NAME_MAX];
	if (!walk_parent(path, &parent, leaf, sizeof(leaf))) {
		return false;
	}
	if (leaf[0] == '\0') {
		if (out) {
			ksnprintf(out->name, sizeof(out->name), "/");
			out->kind = FAT_DIR;
			out->size = 0;
			out->cluster = parent;
		}
		return true;
	}
	if (!dir_find(parent, leaf, out, NULL)) {
		fat_fail("file not found");
		return false;
	}
	return true;
}

bool fat_iter_begin(const char *dir_path, struct fat_iter *it)
{
	struct fat_info inf;
	if (!fat_stat(dir_path, &inf) || inf.kind != FAT_DIR) {
		fat_fail("not a directory");
		return false;
	}
	it->dir_clus = inf.cluster ? inf.cluster : root_clus;
	it->index = 0;
	return true;
}

bool fat_iter_next(struct fat_iter *it, struct fat_info *out)
{
	uint8_t lfn[8][32];
	unsigned nlfn = 0;
	for (;;) {
		uint8_t ent[32];
		int r = dir_get(it->dir_clus, it->index, ent);
		if (r <= 0) {
			return false;
		}
		it->index++;
		if (ent[0] == 0x00) {
			return false;
		}
		if (ent[0] == 0xE5) {
			nlfn = 0;
			continue;
		}
		if (ent[11] == ATTR_LFN) {
			if (nlfn < 8) {
				memcpy(lfn[nlfn++], ent, 32);
			}
			continue;
		}
		if (ent[11] & ATTR_VOL) {
			nlfn = 0;
			continue;
		}
		char shortn[16];
		char longn[FAT_NAME_MAX];
		decode83(ent, shortn, sizeof(shortn));
		longn[0] = '\0';
		if (nlfn) {
			decode_lfn(lfn, nlfn, longn, sizeof(longn));
		}
		nlfn = 0;
		ksnprintf(out->name, sizeof(out->name), "%s", longn[0] ? longn : shortn);
		out->kind = (ent[11] & ATTR_DIR) ? FAT_DIR : FAT_FILE;
		out->size = r32(ent + 28);
		out->cluster = ent_clus(ent);
		if (strcmp(out->name, ".") == 0 || strcmp(out->name, "..") == 0) {
			continue;
		}
		return true;
	}
}

/** Find a free 32-byte slot in `dir_clus`. */
static int dir_free_slot(uint32_t dir_clus, uint32_t *slot)
{
	for (uint32_t i = 0; i < 4096; i++) {
		uint8_t ent[32];
		int r = dir_get(dir_clus, i, ent);
		if (r < 0) {
			return 0;
		}
		if (r == 0) {
			/* Need a new cluster. */
			uint32_t c = dir_clus;
			uint32_t last = c;
			while (c >= 2 && c < EOC) {
				last = c;
				c = fat_get(c);
			}
			uint32_t nw = alloc_clus();
			if (nw == 0) {
				return 0;
			}
			if (fat_set(last, nw) != 0 || fat_set(nw, 0x0FFFFFFFu) != 0) {
				return 0;
			}
			fat_flush();
			*slot = i;
			return 1;
		}
		if (ent[0] == 0x00 || ent[0] == 0xE5) {
			*slot = i;
			return 1;
		}
	}
	fat_fail("directory full");
	return 0;
}

static int dir_add(uint32_t dir_clus, const char *name, uint8_t attr, uint32_t clus, uint32_t size)
{
	uint32_t slot;
	if (!dir_free_slot(dir_clus, &slot)) {
		return 0;
	}
	uint8_t ent[32];
	memset(ent, 0, 32);
	if (!encode83(name, ent)) {
		fat_fail("bad name");
		return 0;
	}
	ent[11] = attr;
	ent_set_clus(ent, clus);
	w32(ent + 28, size);
	return dir_put(dir_clus, slot, ent) == 0;
}

bool fat_read(const char *path, void *buf, uint32_t cap, uint32_t *out_size)
{
	struct fat_info inf;
	if (!fat_stat(path, &inf)) {
		return false;
	}
	if (inf.kind != FAT_FILE) {
		fat_fail("not a file");
		return false;
	}
	uint32_t n = inf.size;
	if (n > cap) {
		n = cap;
	}
	uint8_t *dst = buf;
	uint32_t left = n;
	uint32_t c = inf.cluster;
	while (left > 0 && c >= 2 && c < EOC) {
		if (read_clus(c) != 0) {
			fat_fail("read error");
			return false;
		}
		uint32_t chunk = left < cluster_bytes ? left : cluster_bytes;
		memcpy(dst, clus_buf, chunk);
		dst += chunk;
		left -= chunk;
		c = fat_get(c);
	}
	if (out_size) {
		*out_size = n;
	}
	return true;
}

bool fat_write(const char *path, const void *buf, uint32_t size)
{
	uint32_t parent;
	char leaf[FAT_NAME_MAX];
	if (!walk_parent(path, &parent, leaf, sizeof(leaf)) || leaf[0] == '\0') {
		return false;
	}
	struct fat_info inf;
	uint32_t slot = 0;
	int exists = dir_find(parent, leaf, &inf, &slot);
	if (exists && inf.kind == FAT_DIR) {
		fat_fail("is a directory");
		return false;
	}
	if (exists) {
		free_chain(inf.cluster);
	}
	uint32_t first = 0;
	uint32_t prev = 0;
	const uint8_t *src = buf;
	uint32_t left = size;
	if (left == 0) {
		first = 0;
	}
	while (left > 0) {
		uint32_t c = alloc_clus();
		if (c == 0) {
			return false;
		}
		if (first == 0) {
			first = c;
		}
		if (prev) {
			fat_set(prev, c);
		}
		uint32_t chunk = left < cluster_bytes ? left : cluster_bytes;
		memset(clus_buf, 0, cluster_bytes);
		memcpy(clus_buf, src, chunk);
		if (write_clus(c) != 0) {
			fat_fail("write error");
			return false;
		}
		src += chunk;
		left -= chunk;
		prev = c;
	}
	if (prev) {
		fat_set(prev, 0x0FFFFFFFu);
	}
	fat_flush();
	if (exists) {
		uint8_t ent[32];
		if (dir_get(parent, slot, ent) <= 0) {
			return false;
		}
		ent_set_clus(ent, first);
		w32(ent + 28, size);
		return dir_put(parent, slot, ent) == 0;
	}
	return dir_add(parent, leaf, ATTR_ARC, first, size);
}

bool fat_touch(const char *path)
{
	struct fat_info inf;
	if (fat_stat(path, &inf)) {
		return inf.kind == FAT_FILE;
	}
	last_err[0] = '\0';
	return fat_write(path, "", 0);
}

bool fat_mkdir(const char *path)
{
	uint32_t parent;
	char leaf[FAT_NAME_MAX];
	if (!walk_parent(path, &parent, leaf, sizeof(leaf)) || leaf[0] == '\0') {
		fat_fail("bad path");
		return false;
	}
	struct fat_info inf;
	if (dir_find(parent, leaf, &inf, NULL)) {
		fat_fail("already exists");
		return false;
	}
	uint32_t c = alloc_clus();
	if (c == 0) {
		return false;
	}
	memset(clus_buf, 0, cluster_bytes);
	uint8_t *dot = clus_buf;
	memset(dot, 0, 64);
	encode83(".", dot);
	dot[11] = ATTR_DIR;
	ent_set_clus(dot, c);
	encode83("..", dot + 32);
	dot[32 + 11] = ATTR_DIR;
	ent_set_clus(dot + 32, parent == root_clus ? 0 : parent);
	if (write_clus(c) != 0) {
		return false;
	}
	return dir_add(parent, leaf, ATTR_DIR, c, 0);
}

static int dir_is_empty(uint32_t clus)
{
	for (uint32_t i = 0; i < 4096; i++) {
		uint8_t ent[32];
		int r = dir_get(clus, i, ent);
		if (r <= 0) {
			return 1;
		}
		if (ent[0] == 0x00) {
			return 1;
		}
		if (ent[0] == 0xE5 || ent[11] == ATTR_LFN || (ent[11] & ATTR_VOL)) {
			continue;
		}
		char nm[16];
		decode83(ent, nm, sizeof(nm));
		if (strcmp(nm, ".") == 0 || strcmp(nm, "..") == 0) {
			continue;
		}
		return 0;
	}
	return 1;
}

bool fat_remove(const char *path)
{
	uint32_t parent;
	char leaf[FAT_NAME_MAX];
	uint32_t slot;
	struct fat_info inf;
	if (!walk_parent(path, &parent, leaf, sizeof(leaf)) || leaf[0] == '\0') {
		return false;
	}
	if (!dir_find(parent, leaf, &inf, &slot)) {
		fat_fail("file not found");
		return false;
	}
	if (inf.kind == FAT_DIR) {
		uint32_t c = inf.cluster ? inf.cluster : root_clus;
		if (!dir_is_empty(c)) {
			fat_fail("directory not empty");
			return false;
		}
	}
	uint8_t ent[32];
	if (dir_get(parent, slot, ent) <= 0) {
		return false;
	}
	ent[0] = 0xE5;
	if (dir_put(parent, slot, ent) != 0) {
		return false;
	}
	if (inf.cluster >= 2) {
		free_chain(inf.cluster);
	}
	return true;
}

bool fat_rename(const char *src, const char *dst)
{
	struct fat_info inf;
	if (!fat_stat(src, &inf)) {
		return false;
	}
	uint8_t *tmp = clus_buf;
	/* Copy file bytes through a second walk so we do not need extra RAM. */
	if (inf.kind == FAT_DIR) {
		fat_fail("cannot move directories");
		return false;
	}
	if (inf.size > cluster_bytes) {
		/* Stream copy via fat_copy then remove. */
		if (!fat_copy(src, dst)) {
			return false;
		}
		return fat_remove(src);
	}
	if (inf.size > 0 && inf.cluster >= 2) {
		if (read_clus(inf.cluster) != 0) {
			return false;
		}
	}
	uint8_t hold[4096];
	if (cluster_bytes > sizeof(hold)) {
		if (!fat_copy(src, dst)) {
			return false;
		}
		return fat_remove(src);
	}
	memcpy(hold, tmp, inf.size);
	if (!fat_write(dst, hold, inf.size)) {
		return false;
	}
	return fat_remove(src);
}

bool fat_copy(const char *src, const char *dst)
{
	struct fat_info inf;
	if (!fat_stat(src, &inf)) {
		return false;
	}
	if (inf.kind != FAT_FILE) {
		fat_fail("not a file");
		return false;
	}
	uint32_t parent;
	char leaf[FAT_NAME_MAX];
	if (!walk_parent(dst, &parent, leaf, sizeof(leaf)) || leaf[0] == '\0') {
		return false;
	}
	struct fat_info exists;
	uint32_t slot = 0;
	int had = dir_find(parent, leaf, &exists, &slot);
	if (had && exists.kind == FAT_DIR) {
		fat_fail("is a directory");
		return false;
	}
	if (had) {
		free_chain(exists.cluster);
	}
	uint32_t first = 0;
	uint32_t prev = 0;
	uint32_t left = inf.size;
	uint32_t sc = inf.cluster;
	if (left == 0) {
		first = 0;
	}
	while (left > 0 && sc >= 2 && sc < EOC) {
		if (read_clus(sc) != 0) {
			fat_fail("read error");
			return false;
		}
		uint8_t snap[4096];
		uint32_t chunk = left < cluster_bytes ? left : cluster_bytes;
		if (chunk > sizeof(snap)) {
			fat_fail("cluster too large");
			return false;
		}
		memcpy(snap, clus_buf, chunk);
		uint32_t dc = alloc_clus();
		if (dc == 0) {
			return false;
		}
		if (first == 0) {
			first = dc;
		}
		if (prev) {
			fat_set(prev, dc);
		}
		memset(clus_buf, 0, cluster_bytes);
		memcpy(clus_buf, snap, chunk);
		if (write_clus(dc) != 0) {
			return false;
		}
		left -= chunk;
		prev = dc;
		sc = fat_get(sc);
	}
	if (prev) {
		fat_set(prev, 0x0FFFFFFFu);
	}
	fat_flush();
	if (had) {
		uint8_t ent[32];
		if (dir_get(parent, slot, ent) <= 0) {
			return false;
		}
		ent_set_clus(ent, first);
		w32(ent + 28, inf.size);
		return dir_put(parent, slot, ent) == 0;
	}
	return dir_add(parent, leaf, ATTR_ARC, first, inf.size);
}

uint64_t fat_bytes_total(void)
{
	return (uint64_t)total_clusters * cluster_bytes;
}

uint64_t fat_bytes_free(void)
{
	uint64_t free_c = 0;
	for (uint32_t c = 2; c < total_clusters + 2; c++) {
		if (fat_get(c) == 0) {
			free_c++;
		}
	}
	return free_c * cluster_bytes;
}

/** True if type is a FAT partition we can mount. */
static int is_fat_ptype(uint8_t type)
{
	return type == 0x0B || type == 0x0C || type == 0x0E;
}

/** True if `lba` already looks like a FAT32 BPB (do not mkfs over it). */
static int looks_fat32(struct blkdev *dev, uint32_t lba)
{
	uint8_t bpb[SECTOR];
	if (dev == NULL || dev->read(lba, 1, bpb) != 0) {
		return 0;
	}
	if (bpb[510] != 0x55 || bpb[511] != 0xAA) {
		return 0;
	}
	if (r16(bpb + 11) != SECTOR) {
		return 0;
	}
	uint8_t s = bpb[13];
	if (s == 0 || (s & (s - 1)) != 0 || s > 8) {
		return 0;
	}
	return r32(bpb + 36) != 0 && bpb[16] == 2 && r32(bpb + 44) >= 2;
}

/** Parse BPB at the current volume's part_lba into that volume. */
static int fat_bind(void)
{
	mounted = 0;
	volume[0] = '\0';
	fat_cache_sec = 0xFFFFFFFFu;
	fat_cache_dirty = 0;
	if (bd == NULL) {
		fat_fail("no block device");
		return 0;
	}
	uint8_t bpb[SECTOR];
	if (rd(0, 1, bpb) != 0) {
		fat_fail("cannot read boot sector");
		return 0;
	}
	if (bpb[510] != 0x55 || bpb[511] != 0xAA) {
		fat_fail("not FAT (refusing to format)");
		return 0;
	}
	uint32_t bps = r16(bpb + 11);
	if (bps != SECTOR) {
		ksnprintf(last_err, sizeof(last_err),
			"unsupported sector size %u (part lba %u)", bps, part_lba);
		return 0;
	}
	spc = bpb[13];
	if (spc == 0 || (spc & (spc - 1)) != 0 || spc > 8) {
		fat_fail("unsupported cluster size");
		return 0;
	}
	cluster_bytes = spc * SECTOR;
	uint32_t reserved = r16(bpb + 14);
	uint32_t fats = bpb[16];
	fat_sz = r32(bpb + 36);
	root_clus = r32(bpb + 44);
	if (fat_sz == 0 || fats != 2 || reserved == 0 || root_clus < 2) {
		fat_fail("not FAT32 (refusing to format)");
		return 0;
	}
	fat_begin = reserved;
	data_begin = reserved + fats * fat_sz;
	uint32_t totsec = r32(bpb + 32);
	if (totsec == 0) {
		totsec = r16(bpb + 19);
	}
	if (totsec <= data_begin) {
		fat_fail("bad FAT size");
		return 0;
	}
	total_clusters = (totsec - data_begin) / spc;
	memcpy(volume, bpb + 71, 11);
	volume[11] = '\0';
	for (int i = 10; i >= 0 && volume[i] == ' '; i--) {
		volume[i] = '\0';
	}
	if (clus_buf == NULL) {
		uint32_t dummy = 0;
		clus_buf = phys_alloc(cluster_bytes, &dummy);
		if (clus_buf == NULL) {
			fat_fail("out of memory");
			return 0;
		}
	}
	mounted = 1;
	return 1;
}

/** Identify the first FAT32 partition start LBA on the block device. */
static int find_part(void)
{
	uint8_t mbr[SECTOR];
	if (bd->read(0, 1, mbr) != 0) {
		fat_fail("cannot read sector 0");
		return 0;
	}
	if (mbr[510] != 0x55 || mbr[511] != 0xAA) {
		fat_fail("no 0x55AA signature (not a filesystem we will create)");
		return 0;
	}
	if (mbr[0x1C2] == 0xEE) {
		uint8_t gpt[SECTOR];
		if (bd->read(1, 1, gpt) != 0) {
			return 0;
		}
		if (memcmp(gpt, "EFI PART", 8) != 0) {
			fat_fail("bad GPT");
			return 0;
		}
		uint32_t ent_lba = r32(gpt + 72);
		uint32_t entsz = r32(gpt + 84);
		uint32_t nent = r32(gpt + 80);
		if (entsz < 128 || nent == 0) {
			return 0;
		}
		uint8_t buf[SECTOR];
		if (bd->read(ent_lba, 1, buf) != 0) {
			return 0;
		}
		for (unsigned i = 0; i + entsz <= SECTOR && i / entsz < nent; i += entsz) {
			int empty = 1;
			for (unsigned b = 0; b < 16; b++) {
				if (buf[i + b]) {
					empty = 0;
				}
			}
			if (empty) {
				continue;
			}
			part_lba = r32(buf + i + 32);
			return 1;
		}
		fat_fail("no GPT partition");
		return 0;
	}
	for (unsigned i = 0; i < 4; i++) {
		const uint8_t *e = mbr + 0x1BE + i * 16;
		if (is_fat_ptype(e[4])) {
			part_lba = r32(e + 8);
			return 1;
		}
	}
	part_lba = 0;
	return 1;
}

/** Pack LBA into BIOS CHS bytes (capped at cylinder 1023). */
static void lba_chs(uint32_t lba, uint8_t *head, uint8_t *sec, uint8_t *cyl)
{
	const uint32_t spt = 63;
	const uint32_t heads = 255;
	uint32_t cyln = lba / (spt * heads);
	uint32_t rest = lba % (spt * heads);
	uint32_t h = rest / spt;
	uint32_t s = rest % spt + 1;
	if (cyln > 1023) {
		*head = 0xFE;
		*sec = 0xFF;
		*cyl = 0xFF;
		return;
	}
	*head = (uint8_t)h;
	*sec = (uint8_t)((s & 0x3F) | ((cyln >> 8) << 6));
	*cyl = (uint8_t)(cyln & 0xFF);
}

/** Size a FAT32 for `nsec` partition sectors. */
static int fat_geom(uint32_t nsec, uint32_t *spc_out, uint32_t *fatsz_out, uint32_t *clus_out)
{
	const uint32_t reserved = 32;
	const uint32_t spcs[4] = { 8, 4, 2, 1 };
	for (unsigned i = 0; i < 4; i++) {
		uint32_t try_spc = spcs[i];
		uint32_t fat_len = 32;
		for (unsigned iter = 0; iter < 12; iter++) {
			if (nsec <= reserved + 2u * fat_len + try_spc) {
				break;
			}
			uint32_t clus = (nsec - reserved - 2u * fat_len) / try_spc;
			uint32_t need = ((clus + 2u) * 4u + SECTOR - 1u) / SECTOR;
			if (need <= fat_len) {
				*spc_out = try_spc;
				*fatsz_out = fat_len;
				*clus_out = clus;
				return 1;
			}
			fat_len = need;
		}
	}
	return 0;
}

/** Format FAT32 in `[lba, lba+nsec)` only. Does not touch the MBR. */
static int fat_mkfs(struct blkdev *dev, uint32_t lba, uint32_t nsec)
{
	uint32_t try_spc = 0;
	uint32_t fatsz = 0;
	uint32_t clus = 0;
	if (dev == NULL || dev->write == NULL || nsec < 4096) {
		fat_fail("data region too small");
		return 0;
	}
	if (!fat_geom(nsec, &try_spc, &fatsz, &clus)) {
		fat_fail("cannot size data FAT");
		return 0;
	}
	uint8_t boot[SECTOR];
	memset(boot, 0, SECTOR);
	boot[0] = 0xEB;
	boot[1] = 0x58;
	boot[2] = 0x90;
	memcpy(boot + 3, "MSWIN4.1", 8);
	w16(boot + 11, SECTOR);
	boot[13] = (uint8_t)try_spc;
	w16(boot + 14, 32);
	boot[16] = 2;
	boot[21] = 0xF8;
	w16(boot + 24, 63);
	w16(boot + 26, 255);
	w32(boot + 28, lba);
	w32(boot + 32, nsec);
	w32(boot + 36, fatsz);
	w32(boot + 44, 2);
	w16(boot + 48, 1);
	w16(boot + 50, 6);
	boot[64] = 0x80;
	boot[66] = 0x29;
	w32(boot + 67, 0xA0D10501u);
	memcpy(boot + 71, "DATA       ", 11);
	memcpy(boot + 82, "FAT32   ", 8);
	boot[510] = 0x55;
	boot[511] = 0xAA;
	if (dev->write(lba, 1, boot) != 0) {
		fat_fail("mkfs boot write");
		return 0;
	}
	fat_pump();
	(void)dev->write(lba + 6, 1, boot);
	uint8_t fsinfo[SECTOR];
	memset(fsinfo, 0, SECTOR);
	w32(fsinfo, 0x41615252u);
	w32(fsinfo + 484, 0x61417272u);
	w32(fsinfo + 488, 0xFFFFFFFFu);
	w32(fsinfo + 492, 0xFFFFFFFFu);
	fsinfo[510] = 0x55;
	fsinfo[511] = 0xAA;
	(void)dev->write(lba + 1, 1, fsinfo);
	uint8_t z[SECTOR];
	memset(z, 0, SECTOR);
	uint32_t fat1 = 32;
	for (uint32_t s = 0; s < fatsz * 2u; s++) {
		if (dev->write(lba + fat1 + s, 1, z) != 0) {
			fat_fail("mkfs FAT write");
			return 0;
		}
		if ((s & 15u) == 0) {
			fat_pump();
		}
	}
	/* Media / EOC / root cluster. */
	uint8_t fatsec[SECTOR];
	memset(fatsec, 0, SECTOR);
	w32(fatsec, 0x0FFFFFF8u);
	w32(fatsec + 4, 0x0FFFFFFFu);
	w32(fatsec + 8, 0x0FFFFFFFu);
	if (dev->write(lba + fat1, 1, fatsec) != 0) {
		return 0;
	}
	if (dev->write(lba + fat1 + fatsz, 1, fatsec) != 0) {
		return 0;
	}
	uint32_t data0 = 32 + 2u * fatsz;
	for (uint32_t s = 0; s < try_spc; s++) {
		if (dev->write(lba + data0 + s, 1, z) != 0) {
			fat_fail("mkfs root write");
			return 0;
		}
	}
	/* Volume label in the root directory. */
	uint8_t root[SECTOR];
	memset(root, 0, SECTOR);
	memcpy(root, "DATA       ", 11);
	root[11] = ATTR_VOL;
	if (dev->write(lba + data0, 1, root) != 0) {
		return 0;
	}
	fat_pump();
	return 1;
}

/** Mount an existing data FAT, or create one in leftover MBR space. */
static void fat_prepare_user(struct blkdev *dev)
{
	uint8_t mbr[SECTOR];
	if (dev == NULL || dev->read(0, 1, mbr) != 0) {
		return;
	}
	if (mbr[510] != 0x55 || mbr[511] != 0xAA) {
		return;
	}
	/* GPT: mount a second existing FAT if present; never create. */
	if (mbr[0x1C2] == 0xEE) {
		uint8_t gpt[SECTOR];
		if (dev->read(1, 1, gpt) != 0 || memcmp(gpt, "EFI PART", 8) != 0) {
			return;
		}
		uint32_t ent_lba = r32(gpt + 72);
		uint32_t entsz = r32(gpt + 84);
		uint8_t buf[SECTOR];
		if (entsz < 128 || dev->read(ent_lba, 1, buf) != 0) {
			return;
		}
		uint32_t found[4];
		unsigned nf = 0;
		for (unsigned i = 0; i + entsz <= SECTOR && nf < 4; i += entsz) {
			int empty = 1;
			for (unsigned b = 0; b < 16; b++) {
				if (buf[i + b]) {
					empty = 0;
				}
			}
			if (empty) {
				continue;
			}
			found[nf++] = r32(buf + i + 32);
		}
		if (nf >= 2 && looks_fat32(dev, found[1])) {
			fat_select(FAT_VOL_USR);
			bd = dev;
			part_lba = found[1];
			if (!fat_bind()) {
				vols[FAT_VOL_USR].f_mounted = 0;
			} else {
				tty_set_color(TTY_COL_AUDIO);
				tty_printf("fs: mounted FAT32 data '%s'\n", fat_volume());
				tty_set_color(TTY_COL_FG);
			}
			fat_select(FAT_VOL_SYS);
		}
		return;
	}

	uint32_t sys_lba = vols[FAT_VOL_SYS].f_part_lba;
	int extra = 0;
	int fat_slot = -1;
	uint32_t fat_start = 0;
	uint32_t max_end = 1;
	int empty_slot = -1;
	for (unsigned i = 0; i < 4; i++) {
		uint8_t *e = mbr + 0x1BE + i * 16;
		uint8_t type = e[4];
		uint32_t start = r32(e + 8);
		uint32_t count = r32(e + 12);
		if (type == 0 || count == 0) {
			if (empty_slot < 0) {
				empty_slot = (int)i;
			}
			continue;
		}
		uint32_t end = start + count;
		if (end > max_end) {
			max_end = end;
		}
		if (start == sys_lba) {
			continue;
		}
		if (is_fat_ptype(type) && fat_slot < 0) {
			fat_slot = (int)i;
			fat_start = start;
		} else {
			extra = 1;
		}
	}

	if (fat_slot >= 0 && looks_fat32(dev, fat_start)) {
		fat_select(FAT_VOL_USR);
		bd = dev;
		part_lba = fat_start;
		if (fat_bind()) {
			tty_set_color(TTY_COL_AUDIO);
			tty_printf("fs: mounted FAT32 data '%s'\n", fat_volume());
			tty_set_color(TTY_COL_FG);
		} else {
			vols[FAT_VOL_USR].f_mounted = 0;
		}
		fat_select(FAT_VOL_SYS);
		return;
	}
	if (fat_slot >= 0 || extra) {
		tty_set_color(TTY_COL_DIM);
		tty_puts("fs: other partitions present; not auto-creating a data volume\n");
		tty_set_color(TTY_COL_FG);
		return;
	}
	if (empty_slot < 0) {
		return;
	}
	const uint32_t align = 2048;
	uint32_t start = ((max_end + align - 1u) / align) * align;
	if (start < max_end) {
		start += align;
	}
	if (start >= dev->sectors) {
		return;
	}
	uint64_t left = dev->sectors - start;
	if (left < 32768ull) {
		tty_set_color(TTY_COL_DIM);
		tty_puts("fs: leftover USB space is smaller than 16 MiB; no data partition\n");
		tty_set_color(TTY_COL_FG);
		return;
	}
	if (left > 0xFFFFFFFFull) {
		left = 0xFFFFFFFFull;
	}
	uint32_t nsec = (uint32_t)left;
	if (looks_fat32(dev, start)) {
		tty_set_color(TTY_COL_DIM);
		tty_puts("fs: leftover space already looks like FAT32; not formatting it\n");
		tty_set_color(TTY_COL_FG);
		return;
	}

	uint8_t *ent = mbr + 0x1BE + (unsigned)empty_slot * 16;
	memset(ent, 0, 16);
	uint8_t h0, s0, c0, h1, s1, c1;
	lba_chs(start, &h0, &s0, &c0);
	lba_chs(start + nsec - 1u, &h1, &s1, &c1);
	ent[1] = h0;
	ent[2] = s0;
	ent[3] = c0;
	ent[4] = 0x0C;
	ent[5] = h1;
	ent[6] = s1;
	ent[7] = c1;
	w32(ent + 8, start);
	w32(ent + 12, nsec);
	if (dev->write(0, 1, mbr) != 0) {
		fat_fail("cannot write MBR for data partition");
		return;
	}
	tty_set_color(TTY_COL_AUDIO);
	tty_printf("fs: creating data partition (%llu bytes leftover)\n",
		(unsigned long long)nsec * SECTOR);
	tty_set_color(TTY_COL_FG);
	if (!fat_mkfs(dev, start, nsec)) {
		tty_set_color(TTY_COL_ERR);
		tty_printf("fs: data mkfs failed: %s\n", fat_last_error());
		tty_set_color(TTY_COL_FG);
		return;
	}
	fat_select(FAT_VOL_USR);
	bd = dev;
	part_lba = start;
	if (!fat_bind()) {
		vols[FAT_VOL_USR].f_mounted = 0;
		fat_select(FAT_VOL_SYS);
		return;
	}
	tty_set_color(TTY_COL_AUDIO);
	tty_printf("fs: mounted FAT32 data '%s'\n", fat_volume());
	tty_set_color(TTY_COL_FG);
	fat_select(FAT_VOL_SYS);
}

bool fat_mount(struct blkdev *dev)
{
	last_err[0] = '\0';
	memset(vols, 0, sizeof(vols));
	v = &vols[FAT_VOL_SYS];
	current_vol = FAT_VOL_SYS;
	if (dev == NULL) {
		fat_fail("no block device");
		return false;
	}
	bd = dev;
	if (!find_part()) {
		return false;
	}
	if (!fat_bind()) {
		return false;
	}
	tty_set_color(TTY_COL_AUDIO);
	tty_printf("fs: mounted FAT32 system '%s'\n", fat_volume());
	tty_set_color(TTY_COL_FG);
	fat_prepare_user(dev);
	if (vols[FAT_VOL_USR].f_mounted) {
		fat_select(FAT_VOL_USR);
	} else {
		fat_select(FAT_VOL_SYS);
	}
	return true;
}

const char *fat_vol_name(enum fat_vol_id id)
{
	switch (id) {
	case FAT_VOL_SYS:
	case FAT_VOL_USR:
		if (!vols[id].f_mounted) {
			return "";
		}
		return vols[id].f_volume[0] ? vols[id].f_volume : "UNNAMED";
	default: {
		enum fat_vol_id unknown = id;
		(void)unknown;
		return "";
	}
	}
}

uint64_t fat_vol_bytes_total(enum fat_vol_id id)
{
	switch (id) {
	case FAT_VOL_SYS:
	case FAT_VOL_USR:
		if (!vols[id].f_mounted) {
			return 0;
		}
		return (uint64_t)vols[id].f_total_clusters * vols[id].f_cluster_bytes;
	default: {
		enum fat_vol_id unknown = id;
		(void)unknown;
		return 0;
	}
	}
}

uint64_t fat_vol_bytes_free(enum fat_vol_id id)
{
	switch (id) {
	case FAT_VOL_SYS:
	case FAT_VOL_USR:
		break;
	default: {
		enum fat_vol_id unknown = id;
		(void)unknown;
		return 0;
	}
	}
	if (!vols[id].f_mounted) {
		return 0;
	}
	enum fat_vol_id old = current_vol;
	fat_select(id);
	uint64_t n = fat_bytes_free();
	fat_select(old);
	return n;
}

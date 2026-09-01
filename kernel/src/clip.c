#include "clip.h"
#include "audio.h"
#include "files.h"
#include "fs.h"
#include "klib.h"
#include "phys.h"
#include "tty.h"

#include <stddef.h>

#define CLIP_POOL_FRAMES	(256u * 1024u)
#define CLIP_MAX_FRAMES		(180u * 1024u)
#define SEQ_MAX			32

static struct clip clips[CLIP_MAX];
static int16_t *pool;
static uint32_t pool_cap;
static uint32_t pool_used;
static int16_t *scratch;
static uint32_t scratch_cap;
static uint32_t rng_state = 1u;
static char last_err[80];

struct seq_ev {
	char name[CLIP_NAME];
	uint32_t start;
};

static struct seq_ev seq_ev[SEQ_MAX];
static unsigned seq_n;
static uint32_t seq_len;

/** Print a recoverable music-system error. Never panics. */
static void clip_fail(const char *msg)
{
	ksnprintf(last_err, sizeof(last_err), "%s", msg);
	tty_set_color(TTY_COL_ERR);
	tty_printf("music: %s\n", msg);
	tty_set_color(TTY_COL_FG);
}

/** Clamp a mixer sum into s16. */
static int16_t clamp16(int32_t x)
{
	if (x > 32767) {
		return 32767;
	}
	if (x < -32768) {
		return -32768;
	}
	return (int16_t)x;
}

/** Absolute value of a sample. */
static uint32_t uabs16(int16_t s)
{
	int32_t v = s;
	if (v < 0) {
		v = -v;
	}
	return (uint32_t)v;
}

/** xorshift32; `seed 0` is treated as 1 so the generator cannot stick. */
static uint32_t rnd(void)
{
	uint32_t x = rng_state;
	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	rng_state = x;
	return x;
}

/** Little-endian helpers for WAV headers. */
static uint16_t r16(const uint8_t *p)
{
	return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t r32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void w16(uint8_t *p, uint16_t v)
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

/** Parse an unsigned integer, stopping at the first non-digit. */
static uint32_t parse_u32(const char *s)
{
	uint32_t v = 0;
	if (s == NULL) {
		return 0;
	}
	while (*s >= '0' && *s <= '9') {
		v = v * 10u + (uint32_t)(*s - '0');
		s++;
	}
	return v;
}

/**
 * Parse a signed integer (`-12`, `+7`, `100`).
 * Used for pan, semitones, and raw sample writes.
 */
static int32_t parse_i32(const char *s)
{
	int sign = 1;
	if (s == NULL || *s == '\0') {
		return 0;
	}
	if (*s == '-') {
		sign = -1;
		s++;
	} else if (*s == '+') {
		s++;
	}
	return sign * (int32_t)parse_u32(s);
}

/**
 * Parse a ratio as Q16 (`1.0` → 65536, `0.5` → 32768, `2` → 131072).
 * Rejects zero and absurd values.
 */
static int parse_q16(const char *s, uint32_t *out)
{
	uint32_t whole = 0;
	uint32_t frac = 0;
	uint32_t scale = 1;
	if (s == NULL || *s == '\0') {
		return 0;
	}
	if (*s == '+') {
		s++;
	}
	while (*s >= '0' && *s <= '9') {
		whole = whole * 10u + (uint32_t)(*s - '0');
		s++;
	}
	if (*s == '.') {
		s++;
		while (*s >= '0' && *s <= '9' && scale < 10000) {
			frac = frac * 10u + (uint32_t)(*s - '0');
			scale *= 10u;
			s++;
		}
	}
	while (scale < 10000) {
		frac *= 10u;
		scale *= 10u;
	}
	uint64_t q = (uint64_t)whole * 65536ull + ((uint64_t)frac * 65536ull / 10000ull);
	if (q == 0 || q > (16ull * 65536ull)) {
		return 0;
	}
	*out = (uint32_t)q;
	return 1;
}

/**
 * Amplitude as milles: `0.5` → 500, `1.2` → 1200, `80%` → 800.
 * Unlike the tone parser this may exceed 1.0 (gain, distortion).
 */
static uint32_t parse_amp(const char *s)
{
	uint32_t whole = 0;
	uint32_t frac = 0;
	uint32_t scale = 1;
	if (s == NULL || *s == '\0') {
		return 1000;
	}
	while (*s >= '0' && *s <= '9') {
		whole = whole * 10u + (uint32_t)(*s - '0');
		s++;
	}
	if (*s == '.') {
		s++;
		while (*s >= '0' && *s <= '9' && scale < 1000) {
			frac = frac * 10u + (uint32_t)(*s - '0');
			scale *= 10u;
			s++;
		}
	}
	while (scale < 1000) {
		frac *= 10u;
		scale *= 10u;
	}
	if (*s == '%') {
		return whole * 10u + frac / 100u;
	}
	uint32_t v = whole * 1000u + frac;
	if (v > 8000u) {
		v = 8000u;
	}
	return v;
}

/**
 * Convert a time token into frames at `rate`.
 * `1.5s`, `250ms`, `48000f`, or a bare integer as frames.
 */
static int parse_frames(const char *s, uint32_t rate, uint32_t *out)
{
	uint32_t whole = 0;
	uint32_t frac = 0;
	uint32_t scale = 1;
	const char *p = s;
	if (s == NULL || *s == '\0' || rate == 0) {
		return 0;
	}
	while (*p >= '0' && *p <= '9') {
		whole = whole * 10u + (uint32_t)(*p - '0');
		p++;
	}
	if (*p == '.') {
		p++;
		while (*p >= '0' && *p <= '9' && scale < 1000) {
			frac = frac * 10u + (uint32_t)(*p - '0');
			scale *= 10u;
			p++;
		}
	}
	while (scale < 1000) {
		frac *= 10u;
		scale *= 10u;
	}
	if (p[0] == 'm' && p[1] == 's') {
		*out = (uint32_t)((uint64_t)whole * rate / 1000ull);
		return 1;
	}
	if (p[0] == 's' || p[0] == 'S') {
		*out = (uint32_t)(((uint64_t)whole * 1000ull + frac) * rate / 1000ull);
		return 1;
	}
	if (p[0] == 'f' || p[0] == 'F' || p[0] == '\0') {
		if (s[0] != '\0' && (strchr((char *)s, '.') != NULL) && p[0] == '\0') {
			*out = (uint32_t)(((uint64_t)whole * 1000ull + frac) * rate / 1000ull);
			return 1;
		}
		*out = whole;
		return 1;
	}
	return 0;
}

/** 2^(n/12) as Q16 for n = 0..24 (12-TET). */
static const uint32_t semi_q16[25] = {
	65536, 69433, 73562, 77936, 82570,
	87480, 92682, 98193, 104032, 110218,
	116772, 123715, 131072, 138866, 147123,
	155872, 165140, 174960, 185364, 196386,
	208064, 220436, 233544, 247431, 262144
};

/** Pitch ratio from `1.5`, `+12`, `-5`, or `7st`. */
static int parse_pitch_ratio(const char *s, uint32_t *out)
{
	int sign = 1;
	const char *p = s;
	int st = 0;
	if (s == NULL) {
		return 0;
	}
	if (strchr((char *)s, '.') != NULL && s[0] != '+' && s[0] != '-') {
		return parse_q16(s, out);
	}
	if (*p == '+') {
		p++;
		st = 1;
	} else if (*p == '-') {
		sign = -1;
		p++;
		st = 1;
	}
	size_t n = strlen(p);
	if (n >= 2 && p[n - 2] == 's' && p[n - 1] == 't') {
		st = 1;
	}
	if (!st && s[0] != '+' && s[0] != '-') {
		return parse_q16(s, out);
	}
	int32_t semi = parse_i32(s);
	if (semi < 0) {
		semi = -semi;
		sign = -1;
	}
	if (semi > 24) {
		return 0;
	}
	uint32_t r = semi_q16[semi];
	if (sign < 0) {
		*out = (uint32_t)((65536ull * 65536ull) / r);
	} else {
		*out = r;
	}
	return 1;
}

struct clip *clip_find(const char *name)
{
	if (name == NULL || name[0] == '\0') {
		return NULL;
	}
	for (int i = 0; i < CLIP_MAX; i++) {
		if (clips[i].used && strcmp(clips[i].name, name) == 0) {
			return &clips[i];
		}
	}
	return NULL;
}

/** Hidden working clips (`.play`, `.seq`) stay out of `clips`. */
static int clip_hidden(const struct clip *c)
{
	return c->name[0] == '.';
}

/** True if a token names a DSP operator used by `proc`. */
static int is_op(const char *s);

/** Compact live clips toward the start of the pool. */
static void clip_compact(void)
{
	for (int a = 0; a < CLIP_MAX; a++) {
		for (int b = a + 1; b < CLIP_MAX; b++) {
			if (!clips[a].used || !clips[b].used) {
				continue;
			}
			if (clips[b].pcm < clips[a].pcm) {
				struct clip tmp = clips[a];
				clips[a] = clips[b];
				clips[b] = tmp;
			}
		}
	}
	uint32_t dst = 0;
	for (int i = 0; i < CLIP_MAX; i++) {
		struct clip *c = &clips[i];
		if (!c->used || c->pcm == NULL) {
			continue;
		}
		int16_t *want = pool + dst * 2u;
		if (c->pcm != want) {
			memmove(want, c->pcm, (size_t)c->frames * 4u);
			c->pcm = want;
		}
		dst += c->frames;
	}
	pool_used = dst;
}

/**
 * Allocate `frames` stereo frames from the pool.
 * Compacts first; fails rather than moving memory under the DAC.
 */
static int16_t *pool_alloc(uint32_t frames)
{
	if (frames == 0 || frames > CLIP_MAX_FRAMES) {
		clip_fail("clip too long");
		return NULL;
	}
	if (pool_used + frames <= pool_cap) {
		int16_t *p = pool + pool_used * 2u;
		pool_used += frames;
		return p;
	}
	if (audio_system_get()->play == AUDIO_PLAY_PLAYING) {
		clip_fail("stop playback before this operation (pool is full)");
		return NULL;
	}
	clip_compact();
	if (pool_used + frames > pool_cap) {
		clip_fail("out of clip memory (delete a clip or use a shorter sound)");
		return NULL;
	}
	int16_t *p = pool + pool_used * 2u;
	pool_used += frames;
	return p;
}

/** First free clip slot, or NULL. */
static struct clip *clip_slot(void)
{
	for (int i = 0; i < CLIP_MAX; i++) {
		if (!clips[i].used) {
			return &clips[i];
		}
	}
	clip_fail("all clip slots are in use (8)");
	return NULL;
}

/** Copy a clip name, rejecting empty / overlong identifiers. */
static int clip_set_name(struct clip *c, const char *name)
{
	if (name == NULL || name[0] == '\0') {
		clip_fail("clip name required");
		return 0;
	}
	if (strlen(name) >= CLIP_NAME) {
		clip_fail("clip name too long (15 chars)");
		return 0;
	}
	ksnprintf(c->name, CLIP_NAME, "%s", name);
	return 1;
}

/** Create or replace a named clip with silence (or caller-filled PCM). */
static struct clip *clip_make(const char *name, uint32_t rate, uint32_t frames)
{
	if (rate < 4000 || rate > 192000) {
		clip_fail("sample rate must be 4000..192000");
		return NULL;
	}
	struct clip *c = clip_find(name);
	if (c != NULL) {
		c->used = 0;
		c->pcm = NULL;
		c->frames = 0;
	} else {
		c = clip_slot();
		if (c == NULL) {
			return NULL;
		}
		if (!clip_set_name(c, name)) {
			return NULL;
		}
	}
	int16_t *pcm = pool_alloc(frames);
	if (pcm == NULL) {
		return NULL;
	}
	memset(pcm, 0, (size_t)frames * 4u);
	c->pcm = pcm;
	c->frames = frames;
	c->rate = rate;
	c->used = 1;
	return c;
}

/** Duplicate `src` into `dstname` (in-place if names match). */
static struct clip *clip_copy(struct clip *src, const char *dstname)
{
	if (src == NULL) {
		clip_fail("no such clip");
		return NULL;
	}
	if (dstname == NULL || strcmp(src->name, dstname) == 0) {
		return src;
	}
	struct clip *d = clip_make(dstname, src->rate, src->frames);
	if (d == NULL) {
		return NULL;
	}
	memcpy(d->pcm, src->pcm, (size_t)src->frames * 4u);
	return d;
}

/** Peak absolute sample in a clip. */
static uint32_t clip_peak(const struct clip *c)
{
	uint32_t peak = 0;
	uint32_t n = c->frames * 2u;
	for (uint32_t i = 0; i < n; i++) {
		uint32_t a = uabs16(c->pcm[i]);
		if (a > peak) {
			peak = a;
		}
	}
	return peak;
}

/** Linear interpolate channel `ch` at Q16 index `pos`. */
static int16_t lerp(const int16_t *pcm, uint32_t frames, uint32_t ch, uint32_t pos)
{
	uint32_t i = pos >> 16;
	uint32_t f = pos & 0xFFFFu;
	if (frames == 0) {
		return 0;
	}
	if (i >= frames) {
		i = frames - 1u;
		f = 0;
	}
	int32_t a = pcm[i * 2u + ch];
	int32_t b = (i + 1u < frames) ? pcm[(i + 1u) * 2u + ch] : a;
	return (int16_t)(a + (((b - a) * (int32_t)f) >> 16));
}

/**
 * Resample `src` into a new-length buffer in `scratch`, then replace `c`.
 * `new_frames` is the destination length; content is stretched/squeezed.
 */
static int clip_resample_len(struct clip *c, uint32_t new_frames, uint32_t new_rate)
{
	if (new_frames == 0 || new_frames > CLIP_MAX_FRAMES) {
		clip_fail("result too long");
		return 0;
	}
	if (new_frames > scratch_cap) {
		clip_fail("scratch buffer too small");
		return 0;
	}
	uint32_t step = (uint32_t)(((uint64_t)c->frames << 16) / new_frames);
	uint32_t pos = 0;
	for (uint32_t i = 0; i < new_frames; i++) {
		scratch[i * 2u] = lerp(c->pcm, c->frames, 0, pos);
		scratch[i * 2u + 1u] = lerp(c->pcm, c->frames, 1, pos);
		pos += step;
	}
	c->used = 0;
	c->pcm = NULL;
	int16_t *pcm = pool_alloc(new_frames);
	if (pcm == NULL) {
		return 0;
	}
	memcpy(pcm, scratch, (size_t)new_frames * 4u);
	c->pcm = pcm;
	c->frames = new_frames;
	c->rate = new_rate;
	c->used = 1;
	return 1;
}

/** Decode PCM WAV into stereo s16 at the file's own sample rate. */
static int wav_decode(const uint8_t *data, size_t size, int16_t *dst, uint32_t cap_frames,
	uint32_t *out_frames, uint32_t *out_rate)
{
	if (size < 44 || memcmp(data, "RIFF", 4) != 0 || memcmp(data + 8, "WAVE", 4) != 0) {
		clip_fail("unsupported format (not PCM WAV)");
		return 0;
	}
	size_t off = 12;
	uint16_t fmt = 0;
	uint16_t ch = 0;
	uint32_t rate = 0;
	uint16_t bits = 0;
	const uint8_t *pcm = NULL;
	size_t pcm_bytes = 0;
	while (off + 8 <= size) {
		const uint8_t *ck = data + off;
		uint32_t cksz = r32(ck + 4);
		if (off + 8 + cksz > size) {
			break;
		}
		if (memcmp(ck, "fmt ", 4) == 0 && cksz >= 16) {
			fmt = r16(ck + 8);
			ch = r16(ck + 10);
			rate = r32(ck + 12);
			bits = r16(ck + 22);
		} else if (memcmp(ck, "data", 4) == 0) {
			pcm = ck + 8;
			pcm_bytes = cksz;
		}
		off += 8 + cksz;
		if (cksz & 1u) {
			off++;
		}
	}
	if (fmt != 1 || pcm == NULL || ch < 1 || ch > 2 || (bits != 8 && bits != 16 && bits != 24)) {
		clip_fail("unsupported format (need PCM 8/16/24-bit mono or stereo)");
		return 0;
	}
	if (rate < 4000 || rate > 192000) {
		clip_fail("unsupported sample rate");
		return 0;
	}
	uint32_t src_frames = (uint32_t)(pcm_bytes / (ch * (bits / 8)));
	if (src_frames == 0) {
		clip_fail("empty WAV data");
		return 0;
	}
	if (src_frames > cap_frames) {
		clip_fail("WAV too long");
		return 0;
	}
	for (uint32_t i = 0; i < src_frames; i++) {
		int32_t l;
		int32_t r;
		const uint8_t *sp = pcm + i * ch * (bits / 8);
		if (bits == 8) {
			l = ((int32_t)sp[0] - 128) << 8;
			r = (ch == 2) ? ((int32_t)sp[1] - 128) << 8 : l;
		} else if (bits == 16) {
			l = (int16_t)r16(sp);
			r = (ch == 2) ? (int16_t)r16(sp + 2) : l;
		} else {
			int32_t s24 = (int32_t)(sp[0] | (sp[1] << 8) | (sp[2] << 16));
			if (s24 & 0x800000) {
				s24 |= (int32_t)0xFF000000;
			}
			l = s24 >> 8;
			if (ch == 2) {
				s24 = (int32_t)(sp[3] | (sp[4] << 8) | (sp[5] << 16));
				if (s24 & 0x800000) {
					s24 |= (int32_t)0xFF000000;
				}
				r = s24 >> 8;
			} else {
				r = l;
			}
		}
		dst[i * 2u] = (int16_t)l;
		dst[i * 2u + 1u] = (int16_t)r;
	}
	*out_frames = src_frames;
	*out_rate = rate;
	return 1;
}

/** Encode a clip as PCM s16 stereo WAV into `buf`. */
static int wav_encode(const struct clip *c, uint8_t *buf, uint32_t cap, uint32_t *out_size)
{
	uint32_t data = c->frames * 4u;
	uint32_t total = 44u + data;
	if (total > cap) {
		clip_fail("WAV does not fit the I/O buffer");
		return 0;
	}
	memcpy(buf, "RIFF", 4);
	w32(buf + 4, total - 8u);
	memcpy(buf + 8, "WAVE", 4);
	memcpy(buf + 12, "fmt ", 4);
	w32(buf + 16, 16);
	w16(buf + 20, 1);
	w16(buf + 22, 2);
	w32(buf + 24, c->rate);
	w32(buf + 28, c->rate * 4u);
	w16(buf + 32, 4);
	w16(buf + 34, 16);
	memcpy(buf + 36, "data", 4);
	w32(buf + 40, data);
	memcpy(buf + 44, c->pcm, data);
	*out_size = total;
	return 1;
}

/** Read a WAV from FAT or a Limine module into `name`. */
static struct clip *clip_load_path(const char *path, const char *name)
{
	uint32_t cap = 0;
	uint8_t *iobuf = fs_iobuf(&cap);
	uint32_t n = 0;
	const uint8_t *data = NULL;
	size_t size = 0;
	if (iobuf != NULL && fs_read_file(path, iobuf, cap, &n) && n > 0) {
		data = iobuf;
		size = n;
	} else {
		const struct audio_file *f = files_find(path);
		if (f == NULL) {
			f = files_find(path_basename(path));
		}
		if (f == NULL) {
			clip_fail("file not found");
			return NULL;
		}
		data = f->data;
		size = f->size;
	}
	if (size > (size_t)scratch_cap * 4u) {
		clip_fail("WAV too long");
		return NULL;
	}
	uint32_t frames = 0;
	uint32_t rate = 0;
	if (!wav_decode(data, size, scratch, scratch_cap, &frames, &rate)) {
		return NULL;
	}
	struct clip *c = clip_make(name, rate, frames);
	if (c == NULL) {
		return NULL;
	}
	memcpy(c->pcm, scratch, (size_t)frames * 4u);
	return c;
}

/** Public wrapper so `play` can load a WAV without duplicating FAT/module lookup. */
struct clip *clip_load_file(const char *path, const char *name)
{
	return clip_load_path(path, name);
}

/** Default clip name from a path (`audio/test.wav` → `test`). */
static void name_from_path(const char *path, char *out, size_t n)
{
	const char *base = path_basename(path);
	size_t i = 0;
	while (base[i] && base[i] != '.' && i + 1 < n && i + 1 < CLIP_NAME) {
		char ch = base[i];
		if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z')
			|| (ch >= '0' && ch <= '9') || ch == '_') {
			out[i] = ch;
		} else {
			out[i] = '_';
		}
		i++;
	}
	out[i] = '\0';
	if (out[0] == '\0') {
		ksnprintf(out, n, "%s", "clip");
	}
}

void clip_init(void)
{
	uint32_t phys = 0;
	memset(clips, 0, sizeof(clips));
	pool_cap = CLIP_POOL_FRAMES;
	pool_used = 0;
	pool = phys_alloc(CLIP_POOL_FRAMES * 4u, &phys);
	scratch_cap = CLIP_MAX_FRAMES;
	scratch = phys_alloc(CLIP_MAX_FRAMES * 4u, &phys);
	rng_state = 1u;
	seq_n = 0;
	seq_len = 0;
	if (pool == NULL || scratch == NULL) {
		pool = NULL;
		scratch = NULL;
		pool_cap = 0;
		scratch_cap = 0;
	}
}

static int is_op(const char *s)
{
	static const char *ops[] = {
		"reverse", "gain", "norm", "fade", "pitch", "stretch", "rate",
		"crush", "decimate", "distort", "lpf", "hpf", "bpf", "delay",
		"pan", "vary", NULL
	};
	for (int i = 0; ops[i]; i++) {
		if (strcmp(s, ops[i]) == 0) {
			return 1;
		}
	}
	return 0;
}

/** One-pole low-pass in Q15. `alpha` ≈ 2π fc / (sr + 2π fc), scaled. */
static void filter_lpf(struct clip *c, uint32_t hz)
{
	if (hz < 1) {
		hz = 1;
	}
	uint32_t a = (uint32_t)((uint64_t)hz * 205887ull / (c->rate + hz * 3u + 1u));
	if (a > 32767) {
		a = 32767;
	}
	if (a < 1) {
		a = 1;
	}
	int32_t yl = 0;
	int32_t yr = 0;
	for (uint32_t i = 0; i < c->frames; i++) {
		int32_t xl = c->pcm[i * 2u];
		int32_t xr = c->pcm[i * 2u + 1u];
		yl += ((xl - yl) * (int32_t)a) >> 15;
		yr += ((xr - yr) * (int32_t)a) >> 15;
		c->pcm[i * 2u] = clamp16(yl);
		c->pcm[i * 2u + 1u] = clamp16(yr);
	}
}

/** High-pass = input minus low-pass. */
static void filter_hpf(struct clip *c, uint32_t hz)
{
	if (hz < 1) {
		hz = 1;
	}
	uint32_t a = (uint32_t)((uint64_t)hz * 205887ull / (c->rate + hz * 3u + 1u));
	if (a > 32767) {
		a = 32767;
	}
	int32_t yl = 0;
	int32_t yr = 0;
	for (uint32_t i = 0; i < c->frames; i++) {
		int32_t xl = c->pcm[i * 2u];
		int32_t xr = c->pcm[i * 2u + 1u];
		yl += ((xl - yl) * (int32_t)a) >> 15;
		yr += ((xr - yr) * (int32_t)a) >> 15;
		c->pcm[i * 2u] = clamp16(xl - yl);
		c->pcm[i * 2u + 1u] = clamp16(xr - yr);
	}
}

/** Print clip metadata: duration, rate, frames, peak. */
static void cmd_clip_info(struct clip *c)
{
	uint32_t ms = (uint32_t)((uint64_t)c->frames * 1000ull / c->rate);
	uint32_t peak = clip_peak(c);
	tty_printf("clip %s\n", c->name);
	tty_set_color(TTY_COL_DIM);
	tty_printf("  rate:     %u Hz\n", c->rate);
	tty_printf("  frames:   %u\n", c->frames);
	tty_printf("  duration: %u.%03u s\n", ms / 1000u, ms % 1000u);
	tty_printf("  channels: 2 (s16 interleaved)\n");
	tty_printf("  peak:     %u / 32767\n", peak);
	tty_set_color(TTY_COL_FG);
}

/** List used clips. */
static void cmd_clips(void)
{
	int n = 0;
	tty_puts("clips\n");
	for (int i = 0; i < CLIP_MAX; i++) {
		if (!clips[i].used || clip_hidden(&clips[i])) {
			continue;
		}
		uint32_t ms = (uint32_t)((uint64_t)clips[i].frames * 1000ull / clips[i].rate);
		tty_set_color(TTY_COL_DIM);
		tty_printf("  %-15s %5u Hz  %u frames  %u.%03u s\n",
			clips[i].name, clips[i].rate, clips[i].frames,
			ms / 1000u, ms % 1000u);
		tty_set_color(TTY_COL_FG);
		n++;
	}
	if (n == 0) {
		tty_set_color(TTY_COL_DIM);
		tty_puts("  (none)\n");
		tty_set_color(TTY_COL_FG);
	}
}

/** Reverse the interleaved stereo buffer in place. */
static void op_reverse(struct clip *c)
{
	uint32_t i = 0;
	uint32_t j = c->frames;
	while (i < j) {
		j--;
		int16_t l = c->pcm[i * 2u];
		int16_t r = c->pcm[i * 2u + 1u];
		c->pcm[i * 2u] = c->pcm[j * 2u];
		c->pcm[i * 2u + 1u] = c->pcm[j * 2u + 1u];
		c->pcm[j * 2u] = l;
		c->pcm[j * 2u + 1u] = r;
		i++;
	}
}

/** Scale every sample by `amp` milles (1000 = unity). */
static void op_gain(struct clip *c, uint32_t amp)
{
	uint32_t n = c->frames * 2u;
	for (uint32_t i = 0; i < n; i++) {
		int32_t v = (int32_t)c->pcm[i] * (int32_t)amp / 1000;
		c->pcm[i] = clamp16(v);
	}
}

/** Peak-normalize to `target` milles of full scale (default 0.99). */
static void op_norm(struct clip *c, uint32_t target)
{
	uint32_t peak = clip_peak(c);
	if (peak == 0) {
		return;
	}
	if (target == 0 || target > 1000) {
		target = 990;
	}
	uint32_t want = 32767u * target / 1000u;
	uint32_t n = c->frames * 2u;
	for (uint32_t i = 0; i < n; i++) {
		int32_t v = (int32_t)c->pcm[i] * (int32_t)want / (int32_t)peak;
		c->pcm[i] = clamp16(v);
	}
}

/** Linear fade in or out over `dur` frames. */
static void op_fade(struct clip *c, int fade_in, uint32_t dur)
{
	if (dur > c->frames) {
		dur = c->frames;
	}
	if (dur == 0) {
		return;
	}
	if (fade_in) {
		for (uint32_t i = 0; i < dur; i++) {
			int32_t g = (int32_t)((i * 1000u) / dur);
			c->pcm[i * 2u] = clamp16((int32_t)c->pcm[i * 2u] * g / 1000);
			c->pcm[i * 2u + 1u] = clamp16((int32_t)c->pcm[i * 2u + 1u] * g / 1000);
		}
	} else {
		uint32_t start = c->frames - dur;
		for (uint32_t i = 0; i < dur; i++) {
			int32_t g = (int32_t)(((dur - i) * 1000u) / dur);
			uint32_t k = start + i;
			c->pcm[k * 2u] = clamp16((int32_t)c->pcm[k * 2u] * g / 1000);
			c->pcm[k * 2u + 1u] = clamp16((int32_t)c->pcm[k * 2u + 1u] * g / 1000);
		}
	}
}

/** Reduce bit depth by masking low bits (bitcrush). */
static void op_crush(struct clip *c, uint32_t bits)
{
	if (bits < 1) {
		bits = 1;
	}
	if (bits > 16) {
		bits = 16;
	}
	uint32_t shift = 16u - bits;
	uint32_t n = c->frames * 2u;
	for (uint32_t i = 0; i < n; i++) {
		int32_t s = c->pcm[i];
		s = (s >> (int)shift) << (int)shift;
		c->pcm[i] = (int16_t)s;
	}
}

/** Hold-sample decimation to a lower effective rate (aliasing). */
static void op_decimate(struct clip *c, uint32_t hz)
{
	if (hz < 100) {
		hz = 100;
	}
	if (hz >= c->rate) {
		return;
	}
	uint32_t hold = c->rate / hz;
	if (hold < 2) {
		hold = 2;
	}
	int16_t l = 0;
	int16_t r = 0;
	for (uint32_t i = 0; i < c->frames; i++) {
		if ((i % hold) == 0) {
			l = c->pcm[i * 2u];
			r = c->pcm[i * 2u + 1u];
		}
		c->pcm[i * 2u] = l;
		c->pcm[i * 2u + 1u] = r;
	}
}

/** Hard-clip at `thresh` milles of full scale. */
static void op_distort(struct clip *c, uint32_t thresh)
{
	int32_t t = (int32_t)(32767u * thresh / 1000u);
	if (t < 1) {
		t = 1;
	}
	uint32_t n = c->frames * 2u;
	for (uint32_t i = 0; i < n; i++) {
		int32_t s = c->pcm[i];
		if (s > t) {
			s = t;
		} else if (s < -t) {
			s = -t;
		}
		c->pcm[i] = (int16_t)s;
	}
}

/** Linear pan: -100 left … 0 centre … +100 right. */
static void op_pan(struct clip *c, int32_t pan)
{
	if (pan < -100) {
		pan = -100;
	}
	if (pan > 100) {
		pan = 100;
	}
	int32_t gl = 100 - pan;
	int32_t gr = 100 + pan;
	if (gl > 100) {
		gl = 100;
	}
	if (gr > 100) {
		gr = 100;
	}
	for (uint32_t i = 0; i < c->frames; i++) {
		c->pcm[i * 2u] = clamp16((int32_t)c->pcm[i * 2u] * gl / 100);
		c->pcm[i * 2u + 1u] = clamp16((int32_t)c->pcm[i * 2u + 1u] * gr / 100);
	}
}

/** Add seeded noise at `amt` milles of full scale. */
static void op_vary(struct clip *c, uint32_t amt)
{
	int32_t span = (int32_t)(32767u * amt / 1000u);
	if (span < 1) {
		return;
	}
	uint32_t n = c->frames * 2u;
	for (uint32_t i = 0; i < n; i++) {
		int32_t d = (int32_t)(rnd() % (uint32_t)(span * 2 + 1)) - span;
		c->pcm[i] = clamp16((int32_t)c->pcm[i] + d);
	}
}

/** Delay line with wet mix (milles) and optional feedback (milles). */
static int op_delay(struct clip *c, uint32_t ms, uint32_t wet, uint32_t fb)
{
	uint32_t d = (uint32_t)((uint64_t)ms * c->rate / 1000ull);
	if (d < 1) {
		d = 1;
	}
	if (d > scratch_cap) {
		clip_fail("delay too long");
		return 0;
	}
	memset(scratch, 0, (size_t)d * 4u);
	uint32_t w = 0;
	int32_t dry = (int32_t)(1000u - wet);
	if (dry < 0) {
		dry = 0;
	}
	for (uint32_t i = 0; i < c->frames; i++) {
		int32_t il = c->pcm[i * 2u];
		int32_t ir = c->pcm[i * 2u + 1u];
		int32_t dl = scratch[w * 2u];
		int32_t dr = scratch[w * 2u + 1u];
		c->pcm[i * 2u] = clamp16((il * dry + dl * (int32_t)wet) / 1000);
		c->pcm[i * 2u + 1u] = clamp16((ir * dry + dr * (int32_t)wet) / 1000);
		scratch[w * 2u] = clamp16(il + dl * (int32_t)fb / 1000);
		scratch[w * 2u + 1u] = clamp16(ir + dr * (int32_t)fb / 1000);
		w++;
		if (w >= d) {
			w = 0;
		}
	}
	return 1;
}

/**
 * Apply one DSP operator at argv[0]. Writes the number of consumed
 * tokens (including the verb) to `used`.
 */
static int apply_op(struct clip *c, int argc, char **argv, int *used)
{
	if (argc < 1) {
		return 0;
	}
	const char *op = argv[0];
	if (strcmp(op, "reverse") == 0) {
		op_reverse(c);
		*used = 1;
		return 1;
	}
	if (strcmp(op, "gain") == 0) {
		if (argc < 2) {
			clip_fail("usage: gain <amp>");
			return 0;
		}
		op_gain(c, parse_amp(argv[1]));
		*used = 2;
		return 1;
	}
	if (strcmp(op, "norm") == 0) {
		uint32_t t = 990;
		int n = 1;
		if (argc >= 2 && (argv[1][0] == '.' || (argv[1][0] >= '0' && argv[1][0] <= '9'))) {
			t = parse_amp(argv[1]);
			n = 2;
		}
		op_norm(c, t);
		*used = n;
		return 1;
	}
	if (strcmp(op, "fade") == 0) {
		if (argc < 3) {
			clip_fail("usage: fade in|out <dur>");
			return 0;
		}
		int fade_in = (strcmp(argv[1], "in") == 0);
		if (!fade_in && strcmp(argv[1], "out") != 0) {
			clip_fail("fade needs in or out");
			return 0;
		}
		uint32_t dur = 0;
		if (!parse_frames(argv[2], c->rate, &dur)) {
			clip_fail("bad fade duration");
			return 0;
		}
		op_fade(c, fade_in, dur);
		*used = 3;
		return 1;
	}
	if (strcmp(op, "pitch") == 0) {
		if (argc < 2) {
			clip_fail("usage: pitch <ratio|+semitones> [keep]");
			return 0;
		}
		uint32_t ratio = 0;
		if (!parse_pitch_ratio(argv[1], &ratio) || ratio == 0) {
			clip_fail("bad pitch ratio");
			return 0;
		}
		uint32_t orig = c->frames;
		uint32_t nf = (uint32_t)(((uint64_t)c->frames << 16) / ratio);
		if (nf < 1) {
			nf = 1;
		}
		if (!clip_resample_len(c, nf, c->rate)) {
			return 0;
		}
		int n = 2;
		if (argc >= 3 && strcmp(argv[2], "keep") == 0) {
			if (!clip_resample_len(c, orig, c->rate)) {
				return 0;
			}
			n = 3;
		}
		*used = n;
		return 1;
	}
	if (strcmp(op, "stretch") == 0) {
		if (argc < 2) {
			clip_fail("usage: stretch <ratio>");
			return 0;
		}
		uint32_t ratio = 0;
		if (!parse_q16(argv[1], &ratio)) {
			clip_fail("bad stretch ratio");
			return 0;
		}
		uint32_t nf = (uint32_t)(((uint64_t)c->frames * ratio) >> 16);
		if (nf < 1) {
			nf = 1;
		}
		if (!clip_resample_len(c, nf, c->rate)) {
			return 0;
		}
		*used = 2;
		return 1;
	}
	if (strcmp(op, "rate") == 0) {
		if (argc < 2) {
			clip_fail("usage: rate <hz>");
			return 0;
		}
		uint32_t hz = parse_u32(argv[1]);
		if (hz < 4000 || hz > 192000) {
			clip_fail("rate must be 4000..192000");
			return 0;
		}
		uint32_t nf = (uint32_t)((uint64_t)c->frames * hz / c->rate);
		if (nf < 1) {
			nf = 1;
		}
		if (!clip_resample_len(c, nf, hz)) {
			return 0;
		}
		*used = 2;
		return 1;
	}
	if (strcmp(op, "crush") == 0) {
		if (argc < 2) {
			clip_fail("usage: crush <bits>");
			return 0;
		}
		op_crush(c, parse_u32(argv[1]));
		*used = 2;
		return 1;
	}
	if (strcmp(op, "decimate") == 0) {
		if (argc < 2) {
			clip_fail("usage: decimate <hz>");
			return 0;
		}
		op_decimate(c, parse_u32(argv[1]));
		*used = 2;
		return 1;
	}
	if (strcmp(op, "distort") == 0) {
		if (argc < 2) {
			clip_fail("usage: distort <thresh>");
			return 0;
		}
		op_distort(c, parse_amp(argv[1]));
		*used = 2;
		return 1;
	}
	if (strcmp(op, "lpf") == 0 || strcmp(op, "hpf") == 0 || strcmp(op, "bpf") == 0) {
		if (argc < 2) {
			clip_fail("usage: lpf|hpf|bpf <hz>");
			return 0;
		}
		uint32_t hz = parse_u32(argv[1]);
		if (hz < 1 || hz > 20000) {
			clip_fail("filter hz must be 1..20000");
			return 0;
		}
		if (op[0] == 'l') {
			filter_lpf(c, hz);
		} else if (op[0] == 'h') {
			filter_hpf(c, hz);
		} else {
			filter_hpf(c, hz > 40 ? hz / 2u : 20);
			filter_lpf(c, hz);
		}
		*used = 2;
		return 1;
	}
	if (strcmp(op, "delay") == 0) {
		if (argc < 3) {
			clip_fail("usage: delay <ms> <mix> [feedback]");
			return 0;
		}
		uint32_t ms = parse_u32(argv[1]);
		uint32_t wet = parse_amp(argv[2]);
		uint32_t fb = 0;
		int n = 3;
		if (argc >= 4 && (argv[3][0] == '.' || (argv[3][0] >= '0' && argv[3][0] <= '9'))) {
			fb = parse_amp(argv[3]);
			n = 4;
		}
		if (!op_delay(c, ms, wet, fb)) {
			return 0;
		}
		*used = n;
		return 1;
	}
	if (strcmp(op, "pan") == 0) {
		if (argc < 2) {
			clip_fail("usage: pan <-100..100>");
			return 0;
		}
		op_pan(c, parse_i32(argv[1]));
		*used = 2;
		return 1;
	}
	if (strcmp(op, "vary") == 0) {
		if (argc < 2) {
			clip_fail("usage: vary <amt>");
			return 0;
		}
		op_vary(c, parse_amp(argv[1]));
		*used = 2;
		return 1;
	}
	clip_fail("unknown operation");
	return 0;
}

void music_help(void)
{
	tty_puts("audiOS music (clips are named s16 stereo buffers)\n");
	tty_set_color(TTY_COL_DIM);
	tty_puts("  load <file.wav> [name]     WAV → clip (keeps file rate)\n");
	tty_puts("  save <name> <file.wav>     clip → WAV on the FAT volume\n");
	tty_puts("  clips / clip <name>        list, or print rate/frames/peak\n");
	tty_puts("  new <name> <dur> [rate]    silence clip\n");
	tty_puts("  sample <n> <i> [L] [R]     read or write one stereo frame\n");
	tty_puts("  slice <src> [dst] <a> <b>  cut [a,b) as frames or 0.2s\n");
	tty_puts("  join <dst> <a> <b> ...     concatenate\n");
	tty_puts("  mix <dst> <a> <b> ...      sum (clipping-safe)\n");
	tty_puts("  repeat <src> [dst] <n>     new clip = n copies\n");
	tty_puts("  reverse gain norm fade pitch stretch rate\n");
	tty_puts("  crush decimate distort lpf hpf bpf delay pan vary\n");
	tty_puts("  noise <dst> <dur> [amp]    white noise (uses seed)\n");
	tty_puts("  seed <n>                   deterministic RNG\n");
	tty_puts("  proc <clip> [dst] op ...   chain, e.g. gain 0.5 lpf 2000\n");
	tty_puts("  seq add|list|clear|len|render|play\n");
	tty_puts("  rec <name> <dur>           record the output mix\n");
	tty_puts("  drop <name>                free a clip slot\n");
	tty_puts("  play <clip|file> [loop|n]  loop is until stop\n");
	tty_puts("  script <file>              run commands from a text file\n");
	tty_puts("pitch resamples (length changes); add keep to restore length.\n");
	tty_puts("stretch changes length; rec captures the mix, not analog in.\n");
	tty_set_color(TTY_COL_FG);
}

int music_is_verb(const char *verb)
{
	static const char *v[] = {
		"music", "load", "save", "clips", "clip", "new", "sample",
		"slice", "join", "mix", "repeat", "reverse", "gain", "norm",
		"fade", "pitch", "stretch", "rate", "crush", "decimate",
		"distort", "noise", "seed", "lpf", "hpf", "bpf", "delay",
		"pan", "vary", "proc", "seq", "rec", "drop", NULL
	};
	for (int i = 0; v[i]; i++) {
		if (strcmp(verb, v[i]) == 0) {
			return 1;
		}
	}
	return 0;
}

/** Mix `src` into `dst` starting at `start` dest frames, resampling. */
static void mix_into(struct clip *dst, const struct clip *src, uint32_t start)
{
	uint32_t n = (uint32_t)((uint64_t)src->frames * dst->rate / src->rate);
	for (uint32_t i = 0; i < n; i++) {
		uint32_t di = start + i;
		if (di >= dst->frames) {
			break;
		}
		uint32_t pos = (uint32_t)(((uint64_t)i << 16) * src->rate / dst->rate);
		int32_t l = dst->pcm[di * 2u] + lerp(src->pcm, src->frames, 0, pos);
		int32_t r = dst->pcm[di * 2u + 1u] + lerp(src->pcm, src->frames, 1, pos);
		dst->pcm[di * 2u] = clamp16(l);
		dst->pcm[di * 2u + 1u] = clamp16(r);
	}
}

void music_cmd(int argc, char **argv)
{
	if (pool == NULL || scratch == NULL) {
		clip_fail("clip pool not available");
		return;
	}
	const char *cmd = argv[0];
	if (strcmp(cmd, "music") == 0) {
		music_help();
		return;
	}
	if (strcmp(cmd, "seed") == 0) {
		if (argc < 2) {
			tty_printf("seed %u\n", rng_state);
			return;
		}
		uint32_t s = parse_u32(argv[1]);
		rng_state = s ? s : 1u;
		tty_printf("seed %u\n", rng_state);
		return;
	}
	if (strcmp(cmd, "clips") == 0) {
		cmd_clips();
		return;
	}
	if (strcmp(cmd, "drop") == 0) {
		if (argc < 2) {
			clip_fail("usage: drop <name>");
			return;
		}
		struct clip *c = clip_find(argv[1]);
		if (c == NULL) {
			clip_fail("no such clip");
			return;
		}
		tty_printf("dropped %s\n", c->name);
		c->used = 0;
		c->pcm = NULL;
		c->frames = 0;
		return;
	}
	if (strcmp(cmd, "clip") == 0) {
		if (argc < 2) {
			cmd_clips();
			return;
		}
		struct clip *c = clip_find(argv[1]);
		if (c == NULL) {
			clip_fail("no such clip");
			return;
		}
		cmd_clip_info(c);
		return;
	}
	if (strcmp(cmd, "load") == 0) {
		if (argc < 2) {
			clip_fail("usage: load <file.wav> [name]");
			return;
		}
		char nm[CLIP_NAME];
		const char *name = (argc > 2) ? argv[2] : NULL;
		if (name == NULL) {
			name_from_path(argv[1], nm, sizeof(nm));
			name = nm;
		}
		struct clip *c = clip_load_path(argv[1], name);
		if (c == NULL) {
			return;
		}
		tty_printf("loaded %s → %s (%u Hz, %u frames)\n", argv[1], c->name, c->rate, c->frames);
		return;
	}
	if (strcmp(cmd, "save") == 0) {
		if (argc < 3) {
			clip_fail("usage: save <name> <file.wav>");
			return;
		}
		struct clip *c = clip_find(argv[1]);
		if (c == NULL) {
			clip_fail("no such clip");
			return;
		}
		uint32_t cap = 0;
		uint8_t *iobuf = fs_iobuf(&cap);
		uint32_t n = 0;
		if (iobuf == NULL || !wav_encode(c, iobuf, cap, &n)) {
			return;
		}
		if (!fs_write_file(argv[2], iobuf, n)) {
			clip_fail(fs_error()[0] ? fs_error() : "write failed");
			return;
		}
		tty_printf("saved %s → %s (%u bytes)\n", c->name, argv[2], n);
		return;
	}
	if (strcmp(cmd, "new") == 0) {
		if (argc < 3) {
			clip_fail("usage: new <name> <dur> [rate]");
			return;
		}
		uint32_t rate = audio_system_get()->sample_rate;
		if (argc > 3) {
			rate = parse_u32(argv[3]);
		}
		uint32_t frames = 0;
		if (!parse_frames(argv[2], rate, &frames) || frames == 0) {
			clip_fail("bad duration");
			return;
		}
		if (clip_make(argv[1], rate, frames) == NULL) {
			return;
		}
		tty_printf("new %s %u frames @ %u Hz\n", argv[1], frames, rate);
		return;
	}
	if (strcmp(cmd, "sample") == 0) {
		if (argc < 3) {
			clip_fail("usage: sample <name> <index> [L] [R]");
			return;
		}
		struct clip *c = clip_find(argv[1]);
		if (c == NULL) {
			clip_fail("no such clip");
			return;
		}
		uint32_t i = parse_u32(argv[2]);
		if (i >= c->frames) {
			clip_fail("sample index out of range");
			return;
		}
		if (argc >= 4) {
			int32_t l = parse_i32(argv[3]);
			int32_t r = (argc >= 5) ? parse_i32(argv[4]) : l;
			c->pcm[i * 2u] = clamp16(l);
			c->pcm[i * 2u + 1u] = clamp16(r);
		}
		tty_printf("%s[%u] L=%d R=%d\n", c->name, i,
			(int)c->pcm[i * 2u], (int)c->pcm[i * 2u + 1u]);
		return;
	}
	if (strcmp(cmd, "slice") == 0) {
		int next = 0;
		struct clip *src = NULL;
		if (argc < 4) {
			clip_fail("usage: slice <src> [dst] <start> <end>");
			return;
		}
		src = clip_find(argv[1]);
		if (src == NULL) {
			clip_fail("no such clip");
			return;
		}
		const char *dstn = src->name;
		next = 2;
		if (argc >= 5 && clip_find(argv[2]) == NULL && strchr(argv[2], '.') == NULL
			&& (argv[2][0] < '0' || argv[2][0] > '9')) {
			dstn = argv[2];
			next = 3;
		}
		if (next + 1 >= argc) {
			clip_fail("usage: slice <src> [dst] <start> <end>");
			return;
		}
		uint32_t a = 0;
		uint32_t b = 0;
		if (!parse_frames(argv[next], src->rate, &a) || !parse_frames(argv[next + 1], src->rate, &b)) {
			clip_fail("bad slice range");
			return;
		}
		if (b > src->frames) {
			b = src->frames;
		}
		if (a >= b) {
			clip_fail("empty slice");
			return;
		}
		uint32_t n = b - a;
		if (strcmp(dstn, src->name) == 0) {
			memmove(src->pcm, src->pcm + a * 2u, (size_t)n * 4u);
			src->frames = n;
			tty_printf("sliced %s to %u frames\n", src->name, n);
			return;
		}
		struct clip *d = clip_make(dstn, src->rate, n);
		if (d == NULL) {
			return;
		}
		memcpy(d->pcm, src->pcm + a * 2u, (size_t)n * 4u);
		tty_printf("sliced %s → %s (%u frames)\n", src->name, d->name, n);
		return;
	}
	if (strcmp(cmd, "join") == 0 || strcmp(cmd, "mix") == 0) {
		int is_mix = (cmd[0] == 'm');
		if (argc < 4) {
			clip_fail(is_mix ? "usage: mix <dst> <a> <b> ..." : "usage: join <dst> <a> <b> ...");
			return;
		}
		const char *dstn = argv[1];
		uint32_t rate = 0;
		uint32_t total = 0;
		uint32_t longest = 0;
		for (int i = 2; i < argc; i++) {
			struct clip *s = clip_find(argv[i]);
			if (s == NULL) {
				clip_fail("no such clip");
				return;
			}
			if (rate == 0) {
				rate = s->rate;
			}
			uint32_t nf = (uint32_t)((uint64_t)s->frames * rate / s->rate);
			total += nf;
			if (nf > longest) {
				longest = nf;
			}
		}
		uint32_t out_n = is_mix ? longest : total;
		struct clip *d = clip_make(dstn, rate, out_n);
		if (d == NULL) {
			return;
		}
		if (is_mix) {
			for (int i = 2; i < argc; i++) {
				mix_into(d, clip_find(argv[i]), 0);
			}
			tty_printf("mixed %u clips → %s\n", argc - 2, d->name);
		} else {
			uint32_t off = 0;
			for (int i = 2; i < argc; i++) {
				struct clip *s = clip_find(argv[i]);
				uint32_t nf = (uint32_t)((uint64_t)s->frames * rate / s->rate);
				if (s->rate == rate) {
					memcpy(d->pcm + off * 2u, s->pcm, (size_t)s->frames * 4u);
					off += s->frames;
				} else {
					for (uint32_t k = 0; k < nf && off < d->frames; k++, off++) {
						uint32_t pos = (uint32_t)(((uint64_t)k << 16) * s->rate / rate);
						d->pcm[off * 2u] = lerp(s->pcm, s->frames, 0, pos);
						d->pcm[off * 2u + 1u] = lerp(s->pcm, s->frames, 1, pos);
					}
				}
			}
			d->frames = off;
			tty_printf("joined → %s (%u frames)\n", d->name, d->frames);
		}
		return;
	}
	if (strcmp(cmd, "repeat") == 0) {
		if (argc < 3) {
			clip_fail("usage: repeat <src> [dst] <n>");
			return;
		}
		struct clip *src = clip_find(argv[1]);
		if (src == NULL) {
			clip_fail("no such clip");
			return;
		}
		const char *dstn = src->name;
		const char *ns = argv[2];
		if (argc >= 4) {
			dstn = argv[2];
			ns = argv[3];
		}
		uint32_t times = parse_u32(ns);
		if (times < 1 || times > 64) {
			clip_fail("repeat count must be 1..64");
			return;
		}
		uint64_t nf = (uint64_t)src->frames * times;
		if (nf > CLIP_MAX_FRAMES) {
			clip_fail("result too long");
			return;
		}
		if (scratch_cap < (uint32_t)nf) {
			clip_fail("scratch buffer too small");
			return;
		}
		for (uint32_t t = 0; t < times; t++) {
			memcpy(scratch + t * src->frames * 2u, src->pcm, (size_t)src->frames * 4u);
		}
		struct clip *d = clip_make(dstn, src->rate, (uint32_t)nf);
		if (d == NULL) {
			return;
		}
		memcpy(d->pcm, scratch, (size_t)nf * 4u);
		tty_printf("repeat %s × %u → %s\n", src->name, times, d->name);
		return;
	}
	if (strcmp(cmd, "noise") == 0) {
		if (argc < 3) {
			clip_fail("usage: noise <dst> <dur> [amp]");
			return;
		}
		uint32_t rate = audio_system_get()->sample_rate;
		uint32_t frames = 0;
		if (!parse_frames(argv[2], rate, &frames) || frames == 0) {
			clip_fail("bad duration");
			return;
		}
		uint32_t amp = (argc > 3) ? parse_amp(argv[3]) : 500;
		struct clip *c = clip_make(argv[1], rate, frames);
		if (c == NULL) {
			return;
		}
		int32_t span = (int32_t)(32767u * amp / 1000u);
		for (uint32_t i = 0; i < frames * 2u; i++) {
			int32_t s = (int32_t)(rnd() % (uint32_t)(span * 2 + 1)) - span;
			c->pcm[i] = clamp16(s);
		}
		tty_printf("noise %s %u frames\n", c->name, frames);
		return;
	}
	if (strcmp(cmd, "proc") == 0) {
		if (argc < 3) {
			clip_fail("usage: proc <src> [dst] op args ...");
			return;
		}
		struct clip *src = clip_find(argv[1]);
		if (src == NULL) {
			clip_fail("no such clip");
			return;
		}
		int i = 2;
		struct clip *c = src;
		if (i < argc && !is_op(argv[i])) {
			c = clip_copy(src, argv[i]);
			if (c == NULL) {
				return;
			}
			i++;
		}
		while (i < argc) {
			int used = 0;
			if (!apply_op(c, argc - i, argv + i, &used)) {
				return;
			}
			i += used;
		}
		tty_printf("proc %s (%u frames)\n", c->name, c->frames);
		return;
	}
	if (strcmp(cmd, "seq") == 0) {
		if (argc < 2) {
			clip_fail("usage: seq add|list|clear|len|render|play");
			return;
		}
		const char *sub = argv[1];
		if (strcmp(sub, "clear") == 0) {
			seq_n = 0;
			seq_len = 0;
			tty_puts("seq cleared\n");
			return;
		}
		if (strcmp(sub, "list") == 0) {
			uint32_t er = audio_system_get()->sample_rate;
			tty_puts("seq\n");
			for (unsigned i = 0; i < seq_n; i++) {
				uint32_t ms = (uint32_t)((uint64_t)seq_ev[i].start * 1000ull / er);
				tty_set_color(TTY_COL_DIM);
				tty_printf("  %u  %s  %u.%03u s (%u frames)\n",
					i, seq_ev[i].name, ms / 1000u, ms % 1000u, seq_ev[i].start);
				tty_set_color(TTY_COL_FG);
			}
			if (seq_n == 0) {
				tty_set_color(TTY_COL_DIM);
				tty_puts("  (empty)\n");
				tty_set_color(TTY_COL_FG);
			}
			return;
		}
		if (strcmp(sub, "len") == 0) {
			if (argc < 3) {
				tty_printf("seq len %u frames\n", seq_len);
				return;
			}
			uint32_t er = audio_system_get()->sample_rate;
			if (!parse_frames(argv[2], er, &seq_len)) {
				clip_fail("bad seq length");
				return;
			}
			tty_printf("seq len %u frames\n", seq_len);
			return;
		}
		if (strcmp(sub, "add") == 0) {
			if (argc < 4) {
				clip_fail("usage: seq add <clip> <time>");
				return;
			}
			if (clip_find(argv[2]) == NULL) {
				clip_fail("no such clip");
				return;
			}
			if (seq_n >= SEQ_MAX) {
				clip_fail("seq is full");
				return;
			}
			uint32_t er = audio_system_get()->sample_rate;
			uint32_t st = 0;
			if (!parse_frames(argv[3], er, &st)) {
				clip_fail("bad time");
				return;
			}
			ksnprintf(seq_ev[seq_n].name, CLIP_NAME, "%s", argv[2]);
			seq_ev[seq_n].start = st;
			seq_n++;
			tty_printf("seq add %s @ %u\n", argv[2], st);
			return;
		}
		if (strcmp(sub, "render") == 0 || strcmp(sub, "play") == 0) {
			if (seq_n == 0) {
				clip_fail("seq is empty");
				return;
			}
			uint32_t er = audio_system_get()->sample_rate;
			uint32_t end = seq_len;
			for (unsigned i = 0; i < seq_n; i++) {
				struct clip *s = clip_find(seq_ev[i].name);
				if (s == NULL) {
					continue;
				}
				uint32_t dur = (uint32_t)((uint64_t)s->frames * er / s->rate);
				uint32_t e = seq_ev[i].start + dur;
				if (e > end) {
					end = e;
				}
			}
			if (end == 0) {
				clip_fail("seq has no length");
				return;
			}
			const char *dstn = (argc > 2) ? argv[2] : ".seq";
			if (strcmp(sub, "play") == 0) {
				dstn = ".seq";
			}
			struct clip *d = clip_make(dstn, er, end);
			if (d == NULL) {
				return;
			}
			for (unsigned i = 0; i < seq_n; i++) {
				struct clip *s = clip_find(seq_ev[i].name);
				if (s != NULL) {
					mix_into(d, s, seq_ev[i].start);
				}
			}
			if (strcmp(sub, "play") == 0) {
				if (!audio_play_pcm(d->pcm, d->frames, d->rate, 1)) {
					return;
				}
				tty_puts("playing seq...\n");
			} else {
				tty_printf("seq render → %s (%u frames)\n", d->name, d->frames);
			}
			return;
		}
		clip_fail("seq add|list|clear|len|render|play");
		return;
	}
	if (strcmp(cmd, "rec") == 0) {
		if (argc < 3) {
			clip_fail("usage: rec <name> <dur>");
			return;
		}
		uint32_t rate = audio_system_get()->sample_rate;
		uint32_t frames = 0;
		if (!parse_frames(argv[2], rate, &frames) || frames == 0) {
			clip_fail("bad duration");
			return;
		}
		struct clip *c = clip_make(argv[1], rate, frames);
		if (c == NULL) {
			return;
		}
		if (!audio_rec_start(c->pcm, c->frames)) {
			return;
		}
		tty_printf("recording mix → %s (%u frames)\n", c->name, frames);
		return;
	}

	/* In-place / src [dst] DSP verbs. */
	if (is_op(cmd)) {
		if (argc < 2) {
			clip_fail("clip name required");
			return;
		}
		struct clip *src = clip_find(argv[1]);
		if (src == NULL) {
			clip_fail("no such clip");
			return;
		}
		int i = 2;
		struct clip *c = src;
		if (i < argc && !is_op(argv[i]) && argv[i][0] != '-'
			&& (argv[i][0] < '0' || argv[i][0] > '9')
			&& strchr(argv[i], '.') == NULL
			&& strcmp(argv[i], "in") != 0 && strcmp(argv[i], "out") != 0
			&& strcmp(argv[i], "keep") != 0) {
			c = clip_copy(src, argv[i]);
			if (c == NULL) {
				return;
			}
			i++;
		}
		int used = 0;
		char *opv[8];
		int opa = 0;
		opv[opa++] = argv[0];
		while (i < argc && opa < 8) {
			opv[opa++] = argv[i++];
		}
		if (!apply_op(c, opa, opv, &used)) {
			return;
		}
		tty_printf("%s %s (%u frames)\n", cmd, c->name, c->frames);
		return;
	}
	clip_fail("unknown music command (try music)");
}

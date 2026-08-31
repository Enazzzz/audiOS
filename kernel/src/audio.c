#include "audio.h"
#include "ac97.h"
#include "files.h"
#include "klib.h"
#include "pci.h"
#include "phys.h"
#include "tty.h"
#include "version.h"

#include <stddef.h>

#define TONE_LUT_N		256
#define WAV_MAX_FRAMES		(256u * 1024u)

static const int16_t sine_lut[TONE_LUT_N] = {
	     0,    804,   1608,   2410,   3212,   4011,   4808,   5602,
	  6393,   7179,   7962,   8739,   9512,  10278,  11039,  11793,
	 12539,  13279,  14010,  14732,  15446,  16151,  16846,  17530,
	 18204,  18868,  19519,  20159,  20787,  21403,  22005,  22594,
	 23170,  23731,  24279,  24811,  25329,  25832,  26319,  26790,
	 27245,  27683,  28105,  28510,  28898,  29268,  29621,  29956,
	 30273,  30571,  30852,  31113,  31356,  31580,  31785,  31971,
	 32137,  32285,  32412,  32521,  32609,  32678,  32728,  32757,
	 32767,  32757,  32728,  32678,  32609,  32521,  32412,  32285,
	 32137,  31971,  31785,  31580,  31356,  31113,  30852,  30571,
	 30273,  29956,  29621,  29268,  28898,  28510,  28105,  27683,
	 27245,  26790,  26319,  25832,  25329,  24811,  24279,  23731,
	 23170,  22594,  22005,  21403,  20787,  20159,  19519,  18868,
	 18204,  17530,  16846,  16151,  15446,  14732,  14010,  13279,
	 12539,  11793,  11039,  10278,   9512,   8739,   7962,   7179,
	  6393,   5602,   4808,   4011,   3212,   2410,   1608,    804,
	     0,   -804,  -1608,  -2410,  -3212,  -4011,  -4808,  -5602,
	 -6393,  -7179,  -7962,  -8739,  -9512, -10278, -11039, -11793,
	-12539, -13279, -14010, -14732, -15446, -16151, -16846, -17530,
	-18204, -18868, -19519, -20159, -20787, -21403, -22005, -22594,
	-23170, -23731, -24279, -24811, -25329, -25832, -26319, -26790,
	-27245, -27683, -28105, -28510, -28898, -29268, -29621, -29956,
	-30273, -30571, -30852, -31113, -31356, -31580, -31785, -31971,
	-32137, -32285, -32412, -32521, -32609, -32678, -32728, -32757,
	-32767, -32757, -32728, -32678, -32609, -32521, -32412, -32285,
	-32137, -31971, -31785, -31580, -31356, -31113, -30852, -30571,
	-30273, -29956, -29621, -29268, -28898, -28510, -28105, -27683,
	-27245, -26790, -26319, -25832, -25329, -24811, -24279, -23731,
	-23170, -22594, -22005, -21403, -20787, -20159, -19519, -18868,
	-18204, -17530, -16846, -16151, -15446, -14732, -14010, -13279,
	-12539, -11793, -11039, -10278,  -9512,  -8739,  -7962,  -7179,
	 -6393,  -5602,  -4808,  -4011,  -3212,  -2410,  -1608,   -804
};

static struct audio_system system;
static uint32_t selected_device;
static uint32_t noise_state = 0xA5A5u;
static uint32_t phase;
static uint32_t phase_inc;
static uint32_t amp_mille = 500;
static tone_kind_t tone_kind;
static uint64_t frames_left;
static int source_is_wav;
static int16_t *wav_pcm;
static uint32_t wav_frames;
static uint32_t wav_index;
static uint32_t wav_phys;
static int silent_backend;
static uint32_t rs_pos;
static uint32_t rs_idx;
static int16_t hold_l;
static int16_t hold_r;

static void audio_stop_internal(void);

/** Record a recoverable failure; never panics. */
static void audio_fail(const char *msg)
{
	ksnprintf(system.last_error, sizeof(system.last_error), "%s", msg);
	tty_set_color(TTY_COL_ERR);
	tty_printf("audio: %s\n", msg);
	tty_set_color(TTY_COL_FG);
}

/** Clear the last error string. */
static void audio_ok(void)
{
	system.last_error[0] = '\0';
}

/** xorshift32 for the noise oscillator. */
static uint32_t xorshift(void)
{
	uint32_t x = noise_state;
	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	noise_state = x;
	return x;
}

/** One oscillator sample in s16 at the engine rate. */
static int16_t osc_next(void)
{
	int16_t s = 0;
	switch (tone_kind) {
	case TONE_SINE:
		s = sine_lut[phase >> 24];
		break;
	case TONE_SQUARE:
		s = (phase & 0x80000000u) ? 32767 : -32767;
		break;
	case TONE_SAW:
		s = (int16_t)((int32_t)(phase >> 17) - 32767);
		break;
	case TONE_NOISE:
		s = (int16_t)(xorshift() >> 16);
		break;
	case TONE_SILENCE:
	default:
		s = 0;
		break;
	}
	phase += phase_inc;
	return (int16_t)((int32_t)s * (int32_t)amp_mille / 1000);
}

/** Next stereo engine frame as s16 L/R. */
static void engine_frame(int16_t *l, int16_t *r)
{
	if (system.play != AUDIO_PLAY_PLAYING && system.play != AUDIO_PLAY_STOPPING) {
		*l = 0;
		*r = 0;
		return;
	}
	if (source_is_wav) {
		if (wav_index >= wav_frames) {
			*l = 0;
			*r = 0;
			if (frames_left != UINT64_MAX) {
				system.play = AUDIO_PLAY_STOPPING;
			}
			return;
		}
		*l = wav_pcm[wav_index * 2];
		*r = wav_pcm[wav_index * 2 + 1];
		wav_index++;
		return;
	}
	if (frames_left != UINT64_MAX) {
		if (frames_left == 0) {
			*l = 0;
			*r = 0;
			system.play = AUDIO_PLAY_STOPPING;
			return;
		}
		frames_left--;
	}
	int16_t s = osc_next();
	*l = s;
	*r = s;
}

/** Fill one hardware period of interleaved stereo s16, resampling as needed. */
static void fill_period(int16_t *dst, uint32_t hw_frames)
{
	uint32_t src_rate = system.sample_rate;
	uint32_t dst_rate = silent_backend ? src_rate : ac97_hw_rate();
	if (dst_rate == 0) {
		dst_rate = src_rate;
	}
	uint32_t step = (uint32_t)(((uint64_t)src_rate << 16) / dst_rate);
	for (uint32_t i = 0; i < hw_frames; i++) {
		uint32_t target = rs_pos >> 16;
		while (rs_idx <= target) {
			engine_frame(&hold_l, &hold_r);
			rs_idx++;
			system.frames_played++;
		}
		dst[i * 2] = hold_l;
		dst[i * 2 + 1] = hold_r;
		rs_pos += step;
	}
	if (system.play == AUDIO_PLAY_STOPPING) {
		audio_stop_internal();
	}
}

/** Halt DMA and return the engine to idle. */
static void audio_stop_internal(void)
{
	if (ac97_present()) {
		ac97_stop();
	}
	system.play = AUDIO_PLAY_STOPPED;
	system.stream_count = 0;
}

/** Parse a decimal into milles (0.5 → 500). */
static uint32_t parse_mille(const char *s)
{
	uint32_t whole = 0;
	uint32_t frac = 0;
	uint32_t scale = 1;
	if (s == NULL || *s == '\0') {
		return 500;
	}
	while (*s >= '0' && *s <= '9') {
		whole = whole * 10 + (uint32_t)(*s - '0');
		s++;
	}
	if (*s == '.') {
		s++;
		while (*s >= '0' && *s <= '9' && scale < 1000) {
			frac = frac * 10 + (uint32_t)(*s - '0');
			scale *= 10;
			s++;
		}
	}
	while (scale < 1000) {
		frac *= 10;
		scale *= 10;
	}
	uint32_t v = whole * 1000 + frac;
	if (v > 1000) {
		v = 1000;
	}
	return v;
}

/** Parse 440, 440Hz, 440hz. */
static uint32_t parse_freq(const char *s)
{
	uint32_t v = 0;
	if (s == NULL) {
		return 0;
	}
	while (*s >= '0' && *s <= '9') {
		v = v * 10 + (uint32_t)(*s - '0');
		s++;
	}
	return v;
}

/** Parse 3s, 0.5s, 500ms, or a bare number as seconds. */
static uint32_t parse_duration_ms(const char *s)
{
	if (s == NULL || *s == '\0') {
		return 1000;
	}
	uint32_t whole = 0;
	uint32_t frac = 0;
	uint32_t scale = 1;
	while (*s >= '0' && *s <= '9') {
		whole = whole * 10 + (uint32_t)(*s - '0');
		s++;
	}
	if (*s == '.') {
		s++;
		while (*s >= '0' && *s <= '9' && scale < 1000) {
			frac = frac * 10 + (uint32_t)(*s - '0');
			scale *= 10;
			s++;
		}
	}
	while (scale < 1000) {
		frac *= 10;
		scale *= 10;
	}
	if (s[0] == 'm' && s[1] == 's') {
		return whole;
	}
	if (s[0] == 's' || s[0] == '\0') {
		return whole * 1000 + frac;
	}
	return whole * 1000 + frac;
}

/** Read little-endian integers from a WAV blob. */
static uint16_t r16(const uint8_t *p)
{
	return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t r32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/**
 * Decode PCM WAV into wav_pcm as stereo s16 at the engine rate.
 * Rejects compressed / float formats without crashing.
 */
static bool wav_load(const uint8_t *data, size_t size)
{
	if (size < 44 || memcmp(data, "RIFF", 4) != 0 || memcmp(data + 8, "WAVE", 4) != 0) {
		audio_fail("unsupported format (not PCM WAV)");
		return false;
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
		if (cksz & 1) {
			off++;
		}
	}
	if (fmt != 1 || pcm == NULL || ch < 1 || ch > 2 || (bits != 8 && bits != 16 && bits != 24)) {
		audio_fail("unsupported format (need PCM 8/16/24-bit mono or stereo)");
		return false;
	}
	if (rate < 8000 || rate > 192000) {
		audio_fail("unsupported sample rate");
		return false;
	}
	uint32_t src_frames = (uint32_t)(pcm_bytes / (ch * (bits / 8)));
	if (src_frames == 0) {
		audio_fail("empty WAV data");
		return false;
	}
	uint32_t dst_frames = (uint32_t)((uint64_t)src_frames * system.sample_rate / rate);
	if (dst_frames > WAV_MAX_FRAMES) {
		audio_fail("WAV too long");
		return false;
	}
	if (wav_pcm == NULL) {
		wav_pcm = phys_alloc(WAV_MAX_FRAMES * 4u, &wav_phys);
	}
	if (wav_pcm == NULL) {
		audio_fail("out of memory for WAV");
		return false;
	}
	uint32_t pos = 0;
	uint32_t step = (uint32_t)(((uint64_t)rate << 16) / system.sample_rate);
	for (uint32_t i = 0; i < dst_frames; i++) {
		uint32_t si = pos >> 16;
		if (si >= src_frames) {
			si = src_frames - 1;
		}
		int32_t l;
		int32_t r;
		const uint8_t *sp = pcm + si * ch * (bits / 8);
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
		wav_pcm[i * 2] = (int16_t)l;
		wav_pcm[i * 2 + 1] = (int16_t)r;
		pos += step;
	}
	wav_frames = dst_frames;
	wav_index = 0;
	return true;
}

/** Begin playback on AC97 or the silent backend. */
static bool audio_start_play(void)
{
	system.frames_played = 0;
	system.underruns = 0;
	system.overruns = 0;
	rs_pos = 0;
	rs_idx = 0;
	hold_l = 0;
	hold_r = 0;
	system.play = AUDIO_PLAY_PLAYING;
	system.stream_count = 1;
	/* Only the bound AC97 function can emit samples in v0.0.2. */
	if (ac97_present() && selected_device == ac97_pci_index()) {
		if (!ac97_alive()) {
			system.play = AUDIO_PLAY_STOPPED;
			system.stream_count = 0;
			audio_fail("device disconnection");
			return false;
		}
		silent_backend = 0;
		if (!ac97_start(system.buffer_frames, fill_period)) {
			system.play = AUDIO_PLAY_STOPPED;
			system.stream_count = 0;
			audio_fail("playback failure (DMA start)");
			return false;
		}
		return true;
	}
	silent_backend = 1;
	if (pci_device_count() > 0 && selected_device != ac97_pci_index()) {
		audio_fail("unsupported configuration (HDA output is not in 0.0.2; select AC97)");
		system.play = AUDIO_PLAY_STOPPED;
		system.stream_count = 0;
		return false;
	}
	return true;
}

/** Detect hardware, install defaults, and mark the subsystem ready. */
void audio_init(void)
{
	memset(&system, 0, sizeof(system));
	system.sample_rate = AUDIOS_AUDIO_RATE;
	system.bit_depth = (uint8_t)AUDIOS_AUDIO_BITS;
	system.channels = (uint8_t)AUDIOS_AUDIO_CHANNELS;
	system.buffer_frames = 64;
	system.status = AUDIO_INITIALIZING;
	selected_device = 0;
	if (ac97_init()) {
		uint32_t idx = ac97_pci_index();
		selected_device = (idx == UINT32_MAX) ? 0 : idx;
		ksnprintf(system.device_name, sizeof(system.device_name), "%s", ac97_name());
		system.status = AUDIO_READY;
	} else {
		ksnprintf(system.device_name, sizeof(system.device_name), "%s", "none");
		system.status = AUDIO_READY;	/* engine still runs; output is silent */
		ksnprintf(system.last_error, sizeof(system.last_error),
			"%s", "no AC97 output (playback is silent)");
	}
}

/** Pump DMA so playback continues while the shell stays responsive. */
void audio_service(void)
{
	if (system.play == AUDIO_PLAY_STOPPED) {
		return;
	}
	if (!silent_backend && ac97_present()) {
		if (!ac97_alive()) {
			audio_stop_internal();
			audio_fail("device disconnection");
			return;
		}
		ac97_service(fill_period);
		system.underruns = ac97_underruns();
	}
}

const struct audio_system *audio_system_get(void)
{
	return &system;
}

const char *audio_status_name(audio_status_t status)
{
	switch (status) {
	case AUDIO_UNINITIALIZED:
		return "UNINITIALIZED";
	case AUDIO_INITIALIZING:
		return "INITIALIZING";
	case AUDIO_READY:
		return "READY";
	case AUDIO_FAULT:
		return "FAULT";
	default:
		return "UNKNOWN";
	}
}

const char *audio_play_name(audio_play_state_t play)
{
	switch (play) {
	case AUDIO_PLAY_STOPPED:
		return "stopped";
	case AUDIO_PLAY_PLAYING:
		return "playing";
	case AUDIO_PLAY_STOPPING:
		return "stopping";
	default:
		return "unknown";
	}
}

/** Engine buffer latency in microseconds. */
static uint32_t latency_us(void)
{
	if (system.sample_rate == 0) {
		return 0;
	}
	return system.buffer_frames * 1000000u / system.sample_rate;
}

void audio_print(void)
{
	uint32_t us = latency_us();
	tty_set_color(TTY_COL_FG);
	tty_puts("Audio subsystem\n");
	tty_set_color(TTY_COL_DIM);
	tty_puts("------------------------\n");
	tty_set_color(TTY_COL_AUDIO);
	tty_printf("status:       %s\n", audio_status_name(system.status));
	tty_printf("device:       %s\n", system.device_name);
	tty_printf("sample rate:  %u Hz\n", system.sample_rate);
	tty_printf("format:       %u-bit\n", (unsigned)system.bit_depth);
	tty_printf("channels:     %u\n", (unsigned)system.channels);
	tty_printf("buffer:       %u samples\n", system.buffer_frames);
	tty_printf("latency:      %u.%03u ms\n", us / 1000u, us % 1000u);
	tty_set_color(TTY_COL_FG);
}

void audio_print_status(void)
{
	uint64_t ms = 0;
	if (system.sample_rate) {
		ms = system.frames_played * 1000ull / system.sample_rate;
	}
	tty_puts("Audio status\n");
	tty_set_color(TTY_COL_DIM);
	tty_printf("device:     %s\n", system.device_name);
	tty_printf("sample rate:%u Hz\n", system.sample_rate);
	tty_printf("channels:   %u\n", (unsigned)system.channels);
	tty_printf("buffer:     %u\n", system.buffer_frames);
	tty_printf("playback:   %s\n", audio_play_name(system.play));
	tty_printf("underruns:  %u\n", system.underruns);
	tty_printf("overruns:   %u\n", system.overruns);
	tty_printf("elapsed:    %llu.%03llu s\n",
		(unsigned long long)(ms / 1000u), (unsigned long long)(ms % 1000u));
	if (system.last_error[0]) {
		tty_set_color(TTY_COL_ERR);
		tty_printf("last error: %s\n", system.last_error);
	}
	tty_set_color(TTY_COL_FG);
}

void audio_print_devices(void)
{
	tty_puts("Audio devices\n");
	unsigned n = pci_device_count();
	if (n == 0) {
		tty_set_color(TTY_COL_DIM);
		tty_puts("  (none detected)\n");
		tty_set_color(TTY_COL_FG);
		return;
	}
	for (unsigned i = 0; i < n; i++) {
		const struct pci_device *d = pci_device_at(i);
		const char *kind = "audio";
		if (d->subclass == 0x03) {
			kind = "HDA";
		} else if (d->vendor == 0x8086 && d->device == 0x2415) {
			kind = "AC97";
		} else if (d->subclass == 0x01) {
			kind = "AC97";
		}
		char mark = (i == selected_device) ? '*' : ' ';
		tty_printf(" %c[%u] %s %x:%x class %x:%x\n",
			mark, i, kind, (unsigned)d->vendor, (unsigned)d->device,
			(unsigned)d->class_code, (unsigned)d->subclass);
	}
}

void audio_print_info(void)
{
	tty_puts("Audio device info\n");
	tty_set_color(TTY_COL_DIM);
	tty_printf("selected:  %u (%s)\n", selected_device, system.device_name);
	if (ac97_present()) {
		tty_printf("backend:  AC97 PCM out\n");
		tty_printf("hw rate:   %u Hz\n", ac97_hw_rate());
		tty_printf("hw format: 16-bit stereo PCM\n");
	} else {
		tty_puts("backend:  silent (no output device)\n");
	}
	const struct pci_device *d = pci_device_at(selected_device);
	if (d) {
		tty_printf("pci:       bus %u slot %u func %u irq %u\n",
			(unsigned)d->bus, (unsigned)d->slot, (unsigned)d->func,
			(unsigned)d->irq_line);
		tty_printf("bar0:      0x%x\n", d->bar[0]);
	}
	tty_set_color(TTY_COL_FG);
}

/** Apply `audio set` parameters. Rejects changes during playback. */
static void audio_set(int argc, char **argv)
{
	if (argc < 2) {
		tty_puts("usage: audio set rate|buffer|format|channels|device <value>\n");
		return;
	}
	if (system.play == AUDIO_PLAY_PLAYING) {
		audio_fail("cannot reconfigure during playback (stop first)");
		return;
	}
	const char *key = argv[0];
	const char *val = argv[1];
	if (strcmp(key, "rate") == 0) {
		uint32_t r = parse_freq(val);
		if (r != 44100 && r != 48000 && r != 96000) {
			audio_fail("unsupported configuration (rate: 44100, 48000, 96000)");
			return;
		}
		system.sample_rate = r;
		audio_ok();
		tty_printf("sample rate: %u Hz\n", r);
		return;
	}
	if (strcmp(key, "buffer") == 0) {
		uint32_t b = parse_freq(val);
		if (b != 32 && b != 64 && b != 128 && b != 256) {
			audio_fail("unsupported configuration (buffer: 32, 64, 128, 256)");
			return;
		}
		system.buffer_frames = b;
		audio_ok();
		tty_printf("buffer: %u samples\n", b);
		return;
	}
	if (strcmp(key, "format") == 0 || strcmp(key, "bits") == 0) {
		uint32_t b = parse_freq(val);
		if (b != 16 && b != 24) {
			audio_fail("unsupported configuration (format: 16, 24)");
			return;
		}
		system.bit_depth = (uint8_t)b;
		audio_ok();
		tty_printf("format: %u-bit\n", b);
		return;
	}
	if (strcmp(key, "channels") == 0) {
		uint32_t c = parse_freq(val);
		if (c != 1 && c != 2) {
			audio_fail("unsupported configuration (channels: 1, 2)");
			return;
		}
		system.channels = (uint8_t)c;
		audio_ok();
		tty_printf("channels: %u\n", c);
		return;
	}
	if (strcmp(key, "device") == 0) {
		uint32_t id = parse_freq(val);
		if (id >= pci_device_count() && pci_device_count() > 0) {
			audio_fail("no such device");
			return;
		}
		selected_device = id;
		const struct pci_device *d = pci_device_at(id);
		if (id == ac97_pci_index()) {
			ksnprintf(system.device_name, sizeof(system.device_name), "%s", ac97_name());
		} else if (d) {
			ksnprintf(system.device_name, sizeof(system.device_name),
				"PCI %x:%x", (unsigned)d->vendor, (unsigned)d->device);
		}
		audio_ok();
		tty_printf("device: %s\n", system.device_name);
		return;
	}
	audio_fail("unknown parameter (rate, buffer, format, channels, device)");
}

void audio_cmd(int argc, char **argv)
{
	if (argc <= 1) {
		audio_print();
		return;
	}
	const char *sub = argv[1];
	if (strcmp(sub, "devices") == 0) {
		audio_print_devices();
	} else if (strcmp(sub, "info") == 0) {
		audio_print_info();
	} else if (strcmp(sub, "status") == 0) {
		audio_print_status();
	} else if (strcmp(sub, "set") == 0) {
		audio_set(argc - 2, argv + 2);
	} else if (strcmp(sub, "test") == 0) {
		if (system.play == AUDIO_PLAY_PLAYING) {
			audio_fail("already playing (stop first)");
			return;
		}
		tone_kind = TONE_SINE;
		amp_mille = 500;
		phase = 0;
		phase_inc = (uint32_t)(440ull * 4294967296ull / system.sample_rate);
		frames_left = UINT64_MAX;
		source_is_wav = 0;
		audio_ok();
		if (!audio_start_play()) {
			return;
		}
		tty_set_color(TTY_COL_AUDIO);
		tty_puts("playing...\n");
		tty_set_color(TTY_COL_FG);
		tty_puts("continuous test — stop to end\n");
	} else {
		audio_fail("unknown audio command (devices, info, set, status, test)");
	}
}

void tone_cmd(int argc, char **argv)
{
	if (system.play == AUDIO_PLAY_PLAYING) {
		audio_fail("already playing (stop first)");
		return;
	}
	const char *kind = (argc > 1) ? argv[1] : "sine";
	const char *freq_s = (argc > 2) ? argv[2] : "440";
	const char *amp_s = (argc > 3) ? argv[3] : "0.5";
	const char *dur_s = (argc > 4) ? argv[4] : "1s";
	if (strcmp(kind, "sine") == 0) {
		tone_kind = TONE_SINE;
	} else if (strcmp(kind, "square") == 0) {
		tone_kind = TONE_SQUARE;
	} else if (strcmp(kind, "saw") == 0) {
		tone_kind = TONE_SAW;
	} else if (strcmp(kind, "noise") == 0) {
		tone_kind = TONE_NOISE;
	} else if (strcmp(kind, "silence") == 0) {
		tone_kind = TONE_SILENCE;
	} else {
		audio_fail("unknown signal (sine, square, saw, noise, silence)");
		return;
	}
	uint32_t hz = parse_freq(freq_s);
	if (hz == 0 || hz > 20000) {
		audio_fail("frequency must be 1..20000 Hz");
		return;
	}
	amp_mille = parse_mille(amp_s);
	uint32_t ms = parse_duration_ms(dur_s);
	phase = 0;
	phase_inc = (uint32_t)((uint64_t)hz * 4294967296ull / system.sample_rate);
	frames_left = (uint64_t)system.sample_rate * ms / 1000ull;
	source_is_wav = 0;
	audio_ok();
	if (!audio_start_play()) {
		return;
	}
	tty_set_color(TTY_COL_AUDIO);
	tty_puts("playing...\n");
	tty_set_color(TTY_COL_FG);
}

void play_cmd(int argc, char **argv)
{
	if (argc < 2) {
		tty_puts("usage: play <file.wav>\n");
		return;
	}
	if (system.play == AUDIO_PLAY_PLAYING) {
		audio_fail("already playing (stop first)");
		return;
	}
	const struct audio_file *f = files_find(argv[1]);
	if (f == NULL) {
		audio_fail("file not found");
		return;
	}
	if (!wav_load(f->data, f->size)) {
		return;
	}
	source_is_wav = 1;
	frames_left = wav_frames;
	audio_ok();
	if (!audio_start_play()) {
		return;
	}
	tty_set_color(TTY_COL_AUDIO);
	tty_printf("playing %s...\n", f->name);
	tty_set_color(TTY_COL_FG);
}

void stop_cmd(void)
{
	if (system.play == AUDIO_PLAY_STOPPED) {
		tty_puts("already stopped\n");
		return;
	}
	audio_stop_internal();
	tty_puts("stopped\n");
}

bool audio_stream_open(audio_stream_t **out, uint32_t rate, uint8_t bits, uint8_t channels)
{
	(void)rate;
	(void)bits;
	(void)channels;
	if (out) {
		*out = NULL;
	}
	return system.play == AUDIO_PLAY_PLAYING;
}

void audio_stream_close(audio_stream_t *stream)
{
	(void)stream;
	stop_cmd();
}

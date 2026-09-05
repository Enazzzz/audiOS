#include "audio.h"
#include "ac97.h"
#include "clip.h"
#include "hda.h"
#include "klib.h"
#include "pci.h"
#include "tty.h"
#include "version.h"

#include <stddef.h>

#define TONE_LUT_N		256

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
static int source_is_pcm;
static const int16_t *play_pcm;
static uint32_t play_frames;
static uint64_t play_frac;
static uint64_t play_step;
static int play_loops;
static int16_t *rec_pcm;
static uint32_t rec_frames;
static uint32_t rec_index;
static int rec_own_play;
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

/** Tap the live mix into an in-progress `rec` buffer. */
static void rec_tap(int16_t l, int16_t r)
{
	if (rec_pcm == NULL || rec_index >= rec_frames) {
		return;
	}
	rec_pcm[rec_index * 2u] = l;
	rec_pcm[rec_index * 2u + 1u] = r;
	rec_index++;
	if (rec_index >= rec_frames) {
		rec_pcm = NULL;
		if (rec_own_play) {
			system.play = AUDIO_PLAY_STOPPING;
		}
	}
}

/** Next stereo engine frame as s16 L/R. */
static void engine_frame(int16_t *l, int16_t *r)
{
	if (system.play != AUDIO_PLAY_PLAYING && system.play != AUDIO_PLAY_STOPPING) {
		*l = 0;
		*r = 0;
		rec_tap(0, 0);
		return;
	}
	if (source_is_pcm) {
		for (;;) {
			uint32_t si = (uint32_t)(play_frac >> 16);
			if (si < play_frames) {
				*l = play_pcm[si * 2u];
				*r = play_pcm[si * 2u + 1u];
				play_frac += play_step;
				rec_tap(*l, *r);
				return;
			}
			uint64_t span = (uint64_t)play_frames << 16;
			if (span == 0) {
				break;
			}
			if (play_loops < 0) {
				play_frac -= span;
				continue;
			}
			if (play_loops > 1) {
				play_loops--;
				play_frac -= span;
				continue;
			}
			break;
		}
		*l = 0;
		*r = 0;
		rec_tap(0, 0);
		system.play = AUDIO_PLAY_STOPPING;
		return;
	}
	if (frames_left != UINT64_MAX) {
		if (frames_left == 0) {
			*l = 0;
			*r = 0;
			rec_tap(0, 0);
			system.play = AUDIO_PLAY_STOPPING;
			return;
		}
		frames_left--;
	}
	int16_t s = osc_next();
	*l = s;
	*r = s;
	rec_tap(s, s);
}

/** Fill one hardware period of interleaved stereo s16, resampling as needed. */
static void fill_period(int16_t *dst, uint32_t hw_frames)
{
	uint32_t src_rate = system.sample_rate;
	uint32_t dst_rate = system.sample_rate;
	if (!silent_backend) {
		if (hda_present() && selected_device == hda_pci_index()) {
			dst_rate = hda_hw_rate();
		} else if (ac97_present()) {
			dst_rate = ac97_hw_rate();
		}
	}
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
	if (hda_present()) {
		hda_stop();
	}
	if (ac97_present()) {
		ac97_stop();
	}
	system.play = AUDIO_PLAY_STOPPED;
	system.stream_count = 0;
	rec_pcm = NULL;
	rec_own_play = 0;
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
	/* Prefer HDA (ASRock 960GM-GS3 FX / SB710). AC97 remains a fallback. */
	if (hda_present() && selected_device == hda_pci_index()) {
		if (!hda_alive()) {
			system.play = AUDIO_PLAY_STOPPED;
			system.stream_count = 0;
			audio_fail("device disconnection");
			return false;
		}
		silent_backend = 0;
		if (!hda_start(system.buffer_frames, fill_period)) {
			system.play = AUDIO_PLAY_STOPPED;
			system.stream_count = 0;
			audio_fail("playback failure (HDA DMA start)");
			return false;
		}
		return true;
	}
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
	if (pci_device_count() > 0) {
		audio_fail("unsupported configuration (select the HDA or AC97 device)");
		system.play = AUDIO_PLAY_STOPPED;
		system.stream_count = 0;
		return false;
	}
	return true;
}

/**
 * Play interleaved stereo s16, converting `rate` to the engine rate on the fly.
 * `loops` is 1 = once, -1 = until stop, n = n times.
 */
bool audio_play_pcm(const int16_t *pcm, uint32_t frames, uint32_t rate, int loops)
{
	if (system.play == AUDIO_PLAY_PLAYING) {
		audio_fail("already playing (stop first)");
		return false;
	}
	if (pcm == NULL || frames == 0 || rate == 0) {
		audio_fail("empty clip");
		return false;
	}
	play_pcm = pcm;
	play_frames = frames;
	play_frac = 0;
	play_step = ((uint64_t)rate << 16) / system.sample_rate;
	if (play_step == 0) {
		play_step = 1;
	}
	play_loops = loops < 0 ? -1 : (loops == 0 ? 1 : loops);
	source_is_pcm = 1;
	frames_left = UINT64_MAX;
	audio_ok();
	if (!audio_start_play()) {
		return false;
	}
	return true;
}

/**
 * Record the output mix into `dst`. Starts a silent generator if nothing
 * is already playing so the engine actually produces frames.
 */
bool audio_rec_start(int16_t *dst, uint32_t frames)
{
	if (dst == NULL || frames == 0) {
		audio_fail("empty rec buffer");
		return false;
	}
	rec_pcm = dst;
	rec_frames = frames;
	rec_index = 0;
	if (system.play == AUDIO_PLAY_PLAYING) {
		rec_own_play = 0;
		audio_ok();
		return true;
	}
	rec_own_play = 1;
	tone_kind = TONE_SILENCE;
	source_is_pcm = 0;
	frames_left = frames;
	audio_ok();
	if (!audio_start_play()) {
		rec_pcm = NULL;
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
	system.buffer_frames = 256;
	system.status = AUDIO_INITIALIZING;
	selected_device = 0;
	int have_out = 0;
	if (hda_init()) {
		uint32_t idx = hda_pci_index();
		selected_device = (idx == UINT32_MAX) ? 0 : idx;
		ksnprintf(system.device_name, sizeof(system.device_name), "%s", hda_name());
		have_out = 1;
	}
	if (ac97_init()) {
		if (!have_out) {
			uint32_t idx = ac97_pci_index();
			selected_device = (idx == UINT32_MAX) ? 0 : idx;
			ksnprintf(system.device_name, sizeof(system.device_name), "%s", ac97_name());
			have_out = 1;
		}
	}
	system.status = AUDIO_READY;
	if (!have_out) {
		ksnprintf(system.device_name, sizeof(system.device_name), "%s", "none");
		ksnprintf(system.last_error, sizeof(system.last_error),
			"%s", "no analog output (playback is silent)");
	}
	clip_init();
}

/** Pump DMA so playback continues while the shell stays responsive. */
void audio_service(void)
{
	if (system.play == AUDIO_PLAY_STOPPED) {
		return;
	}
	if (silent_backend) {
		return;
	}
	if (hda_present() && selected_device == hda_pci_index()) {
		if (!hda_alive()) {
			audio_stop_internal();
			audio_fail("device disconnection");
			return;
		}
		hda_service(fill_period);
		system.underruns = hda_underruns();
		return;
	}
	if (ac97_present()) {
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
		if (d->subclass == 0x03 || (d->vendor == 0x1002 && d->device == 0x4383)) {
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
	if (hda_present() && selected_device == hda_pci_index()) {
		tty_printf("backend:  HDA analog out\n");
		tty_printf("board:     ASRock 960GM-GS3 FX (SB710 + ALC662)\n");
		tty_printf("hw rate:   %u Hz\n", hda_hw_rate());
		tty_printf("hw format: 16-bit stereo PCM\n");
	} else if (ac97_present() && selected_device == ac97_pci_index()) {
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
		if (id == hda_pci_index()) {
			ksnprintf(system.device_name, sizeof(system.device_name), "%s", hda_name());
		} else if (id == ac97_pci_index()) {
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

/** Print audio command list (`audio help`). */
static void audio_help(void)
{
	tty_puts("audio commands\n");
	tty_set_color(TTY_COL_DIM);
	tty_puts("  audio              configuration and selected device\n");
	tty_puts("  audio help         this list\n");
	tty_puts("  audio devices      PCI output devices\n");
	tty_puts("  audio info         codec / stream details\n");
	tty_puts("  audio set          rate, buffer, format, channels, device\n");
	tty_puts("  audio status       underruns and frames played\n");
	tty_puts("  audio test         continuous 440 Hz tone until stop\n");
	tty_puts("  tone               sine|square|saw|noise|silence [freq] [amp] [dur]\n");
	tty_puts("  play <clip|file>   play a clip or PCM WAV (loop|n)\n");
	tty_puts("  stop               halt playback\n");
	tty_puts("  music              clip / DSP / seq / rec (music with no args for that list)\n");
	tty_set_color(TTY_COL_FG);
}

void audio_cmd(int argc, char **argv)
{
	if (argc <= 1) {
		audio_print();
		return;
	}
	const char *sub = argv[1];
	if (strcmp(sub, "help") == 0 || strcmp(sub, "?") == 0) {
		audio_help();
	} else if (strcmp(sub, "devices") == 0) {
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
		source_is_pcm = 0;
		audio_ok();
		if (!audio_start_play()) {
			return;
		}
		tty_set_color(TTY_COL_AUDIO);
		tty_puts("playing...\n");
		tty_set_color(TTY_COL_FG);
		tty_puts("continuous test — stop to end\n");
	} else {
		audio_fail("unknown audio command (help, devices, info, set, status, test)");
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
	const char *dur_s = (argc > 4) ? argv[4] : NULL;
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
	phase = 0;
	phase_inc = (uint32_t)((uint64_t)hz * 4294967296ull / system.sample_rate);
	if (dur_s == NULL || dur_s[0] == '\0') {
		frames_left = UINT64_MAX;
	} else {
		uint32_t ms = parse_duration_ms(dur_s);
		frames_left = (uint64_t)system.sample_rate * ms / 1000ull;
	}
	source_is_pcm = 0;
	audio_ok();
	if (!audio_start_play()) {
		return;
	}
	tty_set_color(TTY_COL_AUDIO);
	tty_puts("playing...\n");
	tty_set_color(TTY_COL_FG);
	if (frames_left == UINT64_MAX) {
		tty_puts("continuous — stop to end\n");
	}
}

void play_cmd(int argc, char **argv)
{
	if (argc < 2) {
		tty_puts("usage: play <clip|file.wav> [loop|n]\n");
		return;
	}
	struct clip *c = clip_find(argv[1]);
	if (c == NULL) {
		c = clip_load_file(argv[1], ".play");
		if (c == NULL) {
			return;
		}
	}
	int loops = 1;
	if (argc > 2) {
		if (strcmp(argv[2], "loop") == 0) {
			loops = -1;
		} else {
			int n = 0;
			const char *p = argv[2];
			while (*p >= '0' && *p <= '9') {
				n = n * 10 + (*p - '0');
				p++;
			}
			if (n >= 1) {
				loops = n;
			}
		}
	}
	if (!audio_play_pcm(c->pcm, c->frames, c->rate, loops)) {
		return;
	}
	tty_set_color(TTY_COL_AUDIO);
	tty_printf("playing %s%s...\n", c->name[0] == '.' ? argv[1] : c->name,
		loops < 0 ? " loop" : "");
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

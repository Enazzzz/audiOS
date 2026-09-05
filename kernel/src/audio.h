#ifndef AUDIOS_AUDIO_H
#define AUDIOS_AUDIO_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
	AUDIO_UNINITIALIZED = 0,
	AUDIO_INITIALIZING,
	AUDIO_READY,
	AUDIO_FAULT
} audio_status_t;

typedef enum {
	AUDIO_PLAY_STOPPED = 0,
	AUDIO_PLAY_PLAYING,
	AUDIO_PLAY_STOPPING
} audio_play_state_t;

typedef enum {
	TONE_SINE = 0,
	TONE_SQUARE,
	TONE_SAW,
	TONE_NOISE,
	TONE_SILENCE
} tone_kind_t;

typedef struct audio_stream audio_stream_t;

struct audio_system {
	uint32_t sample_rate;
	uint8_t bit_depth;
	uint8_t channels;
	uint32_t buffer_frames;
	audio_status_t status;
	audio_play_state_t play;
	uint32_t stream_count;
	uint32_t underruns;
	uint32_t overruns;
	uint64_t frames_played;
	char device_name[48];
	char last_error[80];
};

/** Detect hardware, install defaults, and mark the subsystem ready. */
void audio_init(void);

/** Pump DMA. Must run from the shell idle loop. */
void audio_service(void);

const struct audio_system *audio_system_get(void);
const char *audio_status_name(audio_status_t status);
const char *audio_play_name(audio_play_state_t play);

void audio_print(void);
void audio_print_status(void);
void audio_print_devices(void);
void audio_print_info(void);

void audio_cmd(int argc, char **argv);
void tone_cmd(int argc, char **argv);
void play_cmd(int argc, char **argv);
void stop_cmd(void);

/**
 * Play interleaved stereo s16.
 * Replaces anything currently playing (slice preview, F5, `play`).
 * `loops`: 1 = once, -1 = until stop, n = n times.
 */
bool audio_play_pcm(const int16_t *pcm, uint32_t frames, uint32_t rate, int loops);

/** Record the output mix into `dst` for `frames` engine frames. */
bool audio_rec_start(int16_t *dst, uint32_t frames);

/** Record analog mic (1) or line-in (0). Blocks until filled or timeout. */
bool audio_rec_analog(int16_t *dst, uint32_t frames, int mic);

bool audio_stream_open(audio_stream_t **out, uint32_t rate, uint8_t bits, uint8_t channels);
void audio_stream_close(audio_stream_t *stream);

/** Master volume 0–100 (DAC amp + mix scale). F11/F12 bump this. */
void audio_set_volume(unsigned pct);
unsigned audio_volume(void);
void audio_bump_volume(int delta);

/** Capture gain 0–100. */
void audio_set_ingain(unsigned pct);
unsigned audio_ingain(void);

/** Safety limiter (headphones). Speakers can leave this off. */
void audio_set_limiter(int on);
int audio_limiter(void);

/** Pause/resume playback without dropping the clip. */
void audio_pause_toggle(void);
int audio_paused(void);
int audio_is_playing(void);

/** Live meter peaks 0–32767 (line out, mic, line in). */
unsigned audio_peak_out(void);
unsigned audio_peak_mic(void);
unsigned audio_peak_line(void);

/** Draw volume / meter HUD on row 0 (right). Skips serial via a quiet frame. */
void audio_hud_set(int on);
void audio_draw_hud(void);

#endif

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
 * `loops`: 1 = once, -1 = until stop, n = n times.
 */
bool audio_play_pcm(const int16_t *pcm, uint32_t frames, uint32_t rate, int loops);

/** Record the output mix into `dst` for `frames` engine frames. */
bool audio_rec_start(int16_t *dst, uint32_t frames);

bool audio_stream_open(audio_stream_t **out, uint32_t rate, uint8_t bits, uint8_t channels);
void audio_stream_close(audio_stream_t *stream);

#endif

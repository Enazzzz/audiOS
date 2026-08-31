#ifndef AUDIOS_AUDIO_H
#define AUDIOS_AUDIO_H

#include <stdbool.h>
#include <stdint.h>

/*
 * Audio is a core system service, not an application. Milestone 0.1 publishes
 * the configuration and the interfaces later stages will fill in. The engine
 * itself stays in "initializing" until a real device path exists.
 */

typedef enum {
	AUDIO_UNINITIALIZED = 0,
	AUDIO_INITIALIZING,
	AUDIO_READY,
	AUDIO_FAULT
} audio_status_t;

/** Opaque handle reserved for simultaneous streams. */
typedef struct audio_stream audio_stream_t;

struct audio_system {
	uint32_t sample_rate;
	uint8_t bit_depth;
	uint8_t channels;
	audio_status_t status;
	uint32_t stream_count;
};

/** Install default high-resolution stereo parameters. */
void audio_init(void);

/** Return the live audio-system object. */
const struct audio_system *audio_system_get(void);

/** Human-readable status word for the shell. */
const char *audio_status_name(audio_status_t status);

/** Print the `audio` command report. */
void audio_print(void);

/*
 * Foundational stream / graph hooks. 0.1 always fails: no device, no DSP.
 * Later milestones implement these without changing the shell contract.
 */
bool audio_stream_open(audio_stream_t **out, uint32_t rate, uint8_t bits, uint8_t channels);
void audio_stream_close(audio_stream_t *stream);

#endif

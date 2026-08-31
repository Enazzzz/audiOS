#include "audio.h"
#include "tty.h"
#include "version.h"

#include <stddef.h>

static struct audio_system system;

/** Install the default high-resolution stereo configuration. */
void audio_init(void)
{
	system.sample_rate = AUDIOS_AUDIO_RATE;
	system.bit_depth = (uint8_t)AUDIOS_AUDIO_BITS;
	system.channels = (uint8_t)AUDIOS_AUDIO_CHANNELS;
	system.status = AUDIO_INITIALIZING;
	system.stream_count = 0;
}

/** Return the live audio-system object. */
const struct audio_system *audio_system_get(void)
{
	return &system;
}

/** Human-readable status word for the shell. */
const char *audio_status_name(audio_status_t status)
{
	switch (status) {
	case AUDIO_UNINITIALIZED:
		return "uninitialized";
	case AUDIO_INITIALIZING:
		return "initializing";
	case AUDIO_READY:
		return "ready";
	case AUDIO_FAULT:
		return "fault";
	default:
		return "unknown";
	}
}

/** Print the audio subsystem report required by the 0.1 spec. */
void audio_print(void)
{
	tty_set_color(TTY_COL_FG);
	tty_puts("Audio subsystem\n");
	tty_set_color(TTY_COL_AUDIO);
	tty_printf("status: %s\n", audio_status_name(system.status));
	tty_printf("sample rate: %u Hz\n", system.sample_rate);
	tty_printf("bit depth: %u-bit\n", (unsigned)system.bit_depth);
	tty_printf("channels: %u\n", (unsigned)system.channels);
	tty_set_color(TTY_COL_DIM);
	tty_printf("streams: %u  engine: not loaded\n", system.stream_count);
	tty_set_color(TTY_COL_FG);
}

/**
 * Reserved stream constructor. Always fails in 0.1 — there is no device
 * path and no mixer yet. Later milestones fill this in.
 */
bool audio_stream_open(audio_stream_t **out, uint32_t rate, uint8_t bits, uint8_t channels)
{
	(void)rate;
	(void)bits;
	(void)channels;
	if (out != NULL) {
		*out = NULL;
	}
	return false;
}

/** Reserved stream destructor. No-op until streams exist. */
void audio_stream_close(audio_stream_t *stream)
{
	(void)stream;
}

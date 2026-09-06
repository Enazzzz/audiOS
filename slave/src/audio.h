#ifndef AUDIOS_AUDIO_H
#define AUDIOS_AUDIO_H

#include <stdint.h>

/** Probe VIA VT8233(A) then ICH AC97. 48 kHz stereo duplex when present. */
void audio_init(void);

int audio_present(void);
const char *audio_name(void);

/**
 * Start 48-frame (1 ms) ping-pong DMA.
 * `fill` writes playback; `cap` is called with captured stereo s16.
 */
int audio_start_link(void (*fill)(int16_t *dst, uint32_t frames),
	void (*cap)(const int16_t *src, uint32_t frames));

void audio_stop(void);

/** Refill consumed periods. Must run from the idle loop. */
void audio_service(void);

#endif

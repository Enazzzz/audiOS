#ifndef AUDIOS_ALINK_H
#define AUDIOS_ALINK_H

#include <stdint.h>

/** Bring the Pulse MAC up idle (no DMA until `alink_start_slave`). */
void alink_init(void);
/** Pump TX/RX and refill AC97 periods. Call from the idle loop. */
void alink_service(void);
/** Apply pending remote keys and `link cmd` lines. */
void alink_poll(void);
int alink_active(void);
int alink_viewing(void);
void alink_exit_view(void);
int alink_send_key(int key);
/** Fill `frames` of interleaved 48 kHz stereo s16 (and loopback-demodulate). */
void alink_fill(int16_t *dst, uint32_t frames);
/** Analog capture callback. Ignored in loopback. */
void alink_rx_pcm(const int16_t *stereo, uint32_t frames);
void alink_cmd(int argc, char **argv);
/** Start as SLAVE (A7V333). Analog if the codec is up, else loopback. */
int alink_start_slave(void);
/** Shell hook so remote `link cmd` can run a line. */
void alink_set_runner(void (*fn)(const char *line));

#endif

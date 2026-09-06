#ifndef AUDIOS_ALINK_H
#define AUDIOS_ALINK_H

#include <stdint.h>

/**
 * audiOS Audio Link: full-duplex packet modem over analog line-out / line-in.
 *
 * Pulse PHY: 48 kHz 16-bit stereo, 12-bit PAM on L and R independently,
 * 1 ms superframes, ~976 kbit/s payload, sliding-window MAC. Loopback
 * (`link loop`) feeds TX into RX with no ADC (QEMU / `link test`).
 *
 * FX (x86-64) is MASTER by default. A7V333 (i386 slave OS) is SLAVE.
 * Cable: each machine's line-out to the other's line-in (two 3.5 mm leads).
 */

/** Start idle / DMA. Safe to call more than once. */
void alink_init(void);

/** Pump TX/RX state machines. Called from the audio service path. */
void alink_service(void);

/** Apply pending keys, terminal cells, commands, and file I/O (shell loop). */
void alink_poll(void);

/** True while the DAC is carrying the modem (not music). */
int alink_active(void);

/** True while this machine is showing the peer's terminal. */
int alink_viewing(void);

/**
 * Fill `frames` of interleaved stereo s16 at the hardware rate (48 kHz).
 * Also demodulates the same samples when loopback is on.
 */
void alink_fill(int16_t *dst, uint32_t frames);

/** Analog capture callback (HDA line-in). Ignored in loopback. */
void alink_rx_pcm(const int16_t *stereo, uint32_t frames);

/** Leave `link view` (Ctrl-X). */
void alink_exit_view(void);

/** Send one key to the peer (used while viewing). */
int alink_send_key(int key);

/** `link` command. */
void alink_cmd(int argc, char **argv);

#endif

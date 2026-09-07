#ifndef AUDIOS_ALINK_PHY_H
#define AUDIOS_ALINK_PHY_H

#include <stdint.h>

/**
 * audiOS Pulse — baseband stereo PAM at the full 48 kHz sample clock.
 *
 * Each PCM sample carries a 12-bit symbol on L and a 12-bit symbol on R
 * (independent channels, not a mono copy). A 48-sample superframe is 1 ms:
 * 4-sample analog Barker, 2 mid-scale DC pilots, then 42 data samples × 24
 * bits = 126 bytes. CRC-32 sits in the last 4 bytes; 122 bytes are payload.
 *
 * Raw payload rate: 122 bytes × 1000 frames/s = 0.976 Mbit/s.
 * Analog path: short line-out → line-in cable, integer DSP only (no FPU).
 */

/** PCM sample rate the modem is designed for (Hz). */
#define ALPHY_RATE		48000u
/** Superframe length in stereo frames (1.000 ms at 48 kHz). */
#define ALPHY_FRAME_SAMP	48u
/** Barker / sync samples at the start of each superframe. */
#define ALPHY_SYNC_SAMP		4u
/** Mid-scale pilots used to measure analog DC after the Barker. */
#define ALPHY_PILOT_SAMP	2u
/** Data samples after sync + pilots. */
#define ALPHY_DATA_SAMP		(ALPHY_FRAME_SAMP - ALPHY_SYNC_SAMP - ALPHY_PILOT_SAMP)
/** Bytes packed from one superframe, including CRC-32. */
#define ALPHY_RAW		(ALPHY_DATA_SAMP * 3u)
/** Usable payload bytes per superframe. */
#define ALPHY_PAY		(ALPHY_RAW - 4u)
/** TX/RX payload queue depth (16 ms of data). */
#define ALPHY_Q			16u

struct alphy {
	/* TX payload ring. */
	uint8_t txq[ALPHY_Q][ALPHY_PAY];
	unsigned tx_head;
	unsigned tx_tail;
	unsigned tx_count;
	unsigned tx_samp;
	uint8_t tx_raw[ALPHY_RAW];
	uint16_t tx_lfsr;
	uint32_t tx_frames;

	/* RX hunt / lock. */
	int locked;
	unsigned rx_samp;
	int16_t hist_l[ALPHY_SYNC_SAMP];
	int16_t hist_r[ALPHY_SYNC_SAMP];
	unsigned hist_n;
	uint8_t rx_raw[ALPHY_RAW];
	uint16_t rx_lfsr;
	int16_t rx_l[ALPHY_DATA_SAMP];
	int16_t rx_r[ALPHY_DATA_SAMP];
	int32_t dc_l;
	int32_t dc_r;
	int32_t peak;
	uint8_t rxq[ALPHY_Q][ALPHY_PAY];
	unsigned rx_head;
	unsigned rx_tail;
	unsigned rx_count;
	uint32_t rx_ok;
	uint32_t rx_bad;
};

/** Zero queues and start idle (always transmitting). */
void alphy_reset(struct alphy *p);

/** Queue a payload (zero-padded to ALPHY_PAY). Returns 0 if the ring is full. */
int alphy_send(struct alphy *p, const uint8_t *pay, unsigned n);

/** Pop one received payload. Returns bytes copied (ALPHY_PAY) or 0. */
unsigned alphy_recv(struct alphy *p, uint8_t *pay, unsigned cap);

/**
 * Emit `frames` of interleaved stereo s16 at 48 kHz.
 * When `loopback` is set, the same samples are demodulated (QEMU / `link test`).
 */
void alphy_gen(struct alphy *p, int16_t *stereo, uint32_t frames, int loopback);

/** Demodulate analog capture (line-in). Ignored while loopback is on. */
void alphy_adc(struct alphy *p, const int16_t *stereo, uint32_t frames);

/** True after a Barker lock (clock recovered from the analog stream). */
int alphy_locked(const struct alphy *p);

uint32_t alphy_tx_frames(const struct alphy *p);
uint32_t alphy_rx_ok(const struct alphy *p);
uint32_t alphy_rx_bad(const struct alphy *p);
unsigned alphy_tx_space(const struct alphy *p);

#endif

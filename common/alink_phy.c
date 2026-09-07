#include "alink_phy.h"

#include <stddef.h>

/*
 * No libc: the 64-bit kernel is -nostdinc. Tiny copies keep this file
 * shared with the i686 slave and the host PHY test.
 */

/** Copy `n` bytes. */
static void alphy_copy(void *dst, const void *src, unsigned n)
{
	uint8_t *d = (uint8_t *)dst;
	const uint8_t *s = (const uint8_t *)src;
	while (n--) {
		*d++ = *s++;
	}
}

/** Fill `n` bytes with `v`. */
static void alphy_fill(void *dst, uint8_t v, unsigned n)
{
	uint8_t *d = (uint8_t *)dst;
	while (n--) {
		*d++ = v;
	}
}

/** Analog Barker on both channels (outside the 12-bit PAM constellation). */
static const int16_t alphy_barker[ALPHY_SYNC_SAMP] = {
	32767, 32767, -32767, 32767
};

/** IEEE CRC-32 (poly 0xEDB88320), init 0xFFFFFFFF, final invert. */
static uint32_t alphy_crc32(const uint8_t *p, unsigned n)
{
	uint32_t c = 0xFFFFFFFFu;
	unsigned i, b;
	for (i = 0; i < n; i++) {
		c ^= p[i];
		for (b = 0; b < 8u; b++) {
			c = (c & 1u) ? (c >> 1) ^ 0xEDB88320u : (c >> 1);
		}
	}
	return ~c;
}

/** 12-bit Galois LFSR step. Reset per superframe so a slip only kills one ms. */
static uint16_t alphy_lfsr12(uint16_t *s)
{
	uint16_t x = *s;
	uint16_t out = x & 0xFFFu;
	uint16_t lsb = (uint16_t)(x & 1u);
	x = (uint16_t)(x >> 1);
	if (lsb) {
		x ^= 0xB400u;
	}
	*s = x ? x : 0xACE1u;
	return out;
}

/** Map a 12-bit symbol 0..4095 onto a 16-bit PCM sample (±30720). */
static int16_t alphy_map12(unsigned s)
{
	int v = ((int)s - 2048) * 15;
	if (v > 32767) {
		v = 32767;
	}
	if (v < -32767) {
		v = -32767;
	}
	return (int16_t)v;
}

/** Inverse of `alphy_map12` with rounding. */
static unsigned alphy_unmap12(int32_t pcm)
{
	int v;
	if (pcm >= 0) {
		v = (pcm + 7) / 15;
	} else {
		v = -((-pcm + 7) / 15);
	}
	v += 2048;
	if (v < 0) {
		v = 0;
	}
	if (v > 4095) {
		v = 4095;
	}
	return (unsigned)v;
}

/** Pack one stereo 12-bit pair into 3 bytes at data-sample index `i`. */
static void alphy_put_sym(uint8_t *raw, unsigned i, unsigned l, unsigned r)
{
	unsigned o = i * 3u;
	raw[o] = (uint8_t)(l >> 4);
	raw[o + 1u] = (uint8_t)(((l & 15u) << 4) | (r >> 8));
	raw[o + 2u] = (uint8_t)r;
}

/** Unpack the stereo 12-bit pair at data-sample index `i`. */
static void alphy_get_sym(const uint8_t *raw, unsigned i, unsigned *l, unsigned *r)
{
	unsigned o = i * 3u;
	*l = ((unsigned)raw[o] << 4) | ((unsigned)raw[o + 1u] >> 4);
	*r = (((unsigned)raw[o + 1u] & 15u) << 8) | (unsigned)raw[o + 2u];
}

/** True if four samples match the Barker signs at full scale. */
static int alphy_is_barker(const int16_t *s)
{
	unsigned i;
	for (i = 0; i < ALPHY_SYNC_SAMP; i++) {
		int16_t want = alphy_barker[i];
		if (want > 0) {
			if (s[i] < 28000) {
				return 0;
			}
		} else if (s[i] > -28000) {
			return 0;
		}
	}
	return 1;
}

/** Load the next payload (or an idle zero frame) and stamp CRC-32. */
static void alphy_load_tx(struct alphy *p)
{
	uint32_t crc;
	if (p->tx_count > 0) {
		alphy_copy(p->tx_raw, p->txq[p->tx_tail], ALPHY_PAY);
		p->tx_tail = (p->tx_tail + 1u) % ALPHY_Q;
		p->tx_count--;
	} else {
		alphy_fill(p->tx_raw, 0, ALPHY_PAY);
	}
	crc = alphy_crc32(p->tx_raw, ALPHY_PAY);
	p->tx_raw[ALPHY_PAY] = (uint8_t)crc;
	p->tx_raw[ALPHY_PAY + 1u] = (uint8_t)(crc >> 8);
	p->tx_raw[ALPHY_PAY + 2u] = (uint8_t)(crc >> 16);
	p->tx_raw[ALPHY_PAY + 3u] = (uint8_t)(crc >> 24);
	p->tx_lfsr = 0xACE1u;
}

/** Emit one stereo sample of the current superframe. */
static void alphy_tx_one(struct alphy *p, int16_t *l, int16_t *r)
{
	if (p->tx_samp == 0) {
		alphy_load_tx(p);
	}
	if (p->tx_samp < ALPHY_SYNC_SAMP) {
		*l = alphy_barker[p->tx_samp];
		*r = alphy_barker[p->tx_samp];
	} else if (p->tx_samp < ALPHY_SYNC_SAMP + ALPHY_PILOT_SAMP) {
		/* Mid-scale pilots: analog DC measurement, not payload. */
		*l = 0;
		*r = 0;
	} else {
		unsigned di = p->tx_samp - ALPHY_SYNC_SAMP - ALPHY_PILOT_SAMP;
		unsigned sl, sr;
		if (di == 0) {
			p->tx_lfsr = 0xACE1u;
		}
		alphy_get_sym(p->tx_raw, di, &sl, &sr);
		sl ^= alphy_lfsr12(&p->tx_lfsr);
		sr ^= alphy_lfsr12(&p->tx_lfsr);
		*l = alphy_map12(sl);
		*r = alphy_map12(sr);
	}
	p->tx_samp++;
	if (p->tx_samp >= ALPHY_FRAME_SAMP) {
		p->tx_samp = 0;
		p->tx_frames++;
	}
}

/**
 * Slice a full data superframe: subtract the analog DC measured on the
 * mid-scale pilots, descramble, CRC-32.
 */
static void alphy_rx_commit(struct alphy *p)
{
	unsigned i, sl, sr;
	uint32_t crc, got;
	uint16_t lfsr = 0xACE1u;

	alphy_fill(p->rx_raw, 0, sizeof(p->rx_raw));
	for (i = 0; i < ALPHY_DATA_SAMP; i++) {
		int32_t xl = (int32_t)p->rx_l[i] - p->dc_l;
		int32_t xr = (int32_t)p->rx_r[i] - p->dc_r;
		sl = alphy_unmap12(xl) ^ alphy_lfsr12(&lfsr);
		sr = alphy_unmap12(xr) ^ alphy_lfsr12(&lfsr);
		alphy_put_sym(p->rx_raw, i, sl, sr);
	}
	crc = alphy_crc32(p->rx_raw, ALPHY_PAY);
	got = (uint32_t)p->rx_raw[ALPHY_PAY]
		| ((uint32_t)p->rx_raw[ALPHY_PAY + 1u] << 8)
		| ((uint32_t)p->rx_raw[ALPHY_PAY + 2u] << 16)
		| ((uint32_t)p->rx_raw[ALPHY_PAY + 3u] << 24);
	if (crc != got) {
		p->rx_bad++;
		p->locked = 0;
		p->hist_n = 0;
		return;
	}
	if (p->rx_count < ALPHY_Q) {
		alphy_copy(p->rxq[p->rx_head], p->rx_raw, ALPHY_PAY);
		p->rx_head = (p->rx_head + 1u) % ALPHY_Q;
		p->rx_count++;
		p->rx_ok++;
	}
}

/**
 * Hunt the analog Barker, then buffer 44 data samples for a block slice.
 * Mean removal happens at commit so a mid-frame IIR cannot walk the PAM.
 */
static void alphy_rx_one(struct alphy *p, int16_t l, int16_t r)
{
	int32_t mag = l < 0 ? -(int32_t)l : (int32_t)l;
	if (mag > p->peak) {
		p->peak = mag;
	} else if (p->peak > 64) {
		p->peak -= 64;
	}

	if (!p->locked) {
		if (p->hist_n < ALPHY_SYNC_SAMP) {
			p->hist_l[p->hist_n] = l;
			p->hist_r[p->hist_n] = r;
			p->hist_n++;
		} else {
			unsigned i;
			for (i = 0; i < ALPHY_SYNC_SAMP - 1u; i++) {
				p->hist_l[i] = p->hist_l[i + 1u];
				p->hist_r[i] = p->hist_r[i + 1u];
			}
			p->hist_l[ALPHY_SYNC_SAMP - 1u] = l;
			p->hist_r[ALPHY_SYNC_SAMP - 1u] = r;
		}
		if (p->hist_n == ALPHY_SYNC_SAMP && alphy_is_barker(p->hist_l)
			&& alphy_is_barker(p->hist_r)) {
			p->locked = 1;
			p->rx_samp = ALPHY_SYNC_SAMP;
		}
		return;
	}

	if (p->rx_samp < ALPHY_SYNC_SAMP) {
		p->hist_l[p->rx_samp] = l;
		p->hist_r[p->rx_samp] = r;
		p->rx_samp++;
		if (p->rx_samp == ALPHY_SYNC_SAMP) {
			if (!alphy_is_barker(p->hist_l) || !alphy_is_barker(p->hist_r)) {
				p->locked = 0;
				p->hist_n = 0;
				p->rx_bad++;
				return;
			}
			p->dc_l = 0;
			p->dc_r = 0;
		}
		return;
	}

	if (p->rx_samp < ALPHY_SYNC_SAMP + ALPHY_PILOT_SAMP) {
		p->dc_l += l;
		p->dc_r += r;
		p->rx_samp++;
		if (p->rx_samp == ALPHY_SYNC_SAMP + ALPHY_PILOT_SAMP) {
			p->dc_l /= (int32_t)ALPHY_PILOT_SAMP;
			p->dc_r /= (int32_t)ALPHY_PILOT_SAMP;
		}
		return;
	}

	p->rx_l[p->rx_samp - ALPHY_SYNC_SAMP - ALPHY_PILOT_SAMP] = l;
	p->rx_r[p->rx_samp - ALPHY_SYNC_SAMP - ALPHY_PILOT_SAMP] = r;
	p->rx_samp++;
	if (p->rx_samp >= ALPHY_FRAME_SAMP) {
		alphy_rx_commit(p);
		p->rx_samp = 0;
	}
}

void alphy_reset(struct alphy *p)
{
	alphy_fill(p, 0, sizeof(*p));
	p->tx_lfsr = 0xACE1u;
	p->rx_lfsr = 0xACE1u;
}

int alphy_send(struct alphy *p, const uint8_t *pay, unsigned n)
{
	uint8_t *slot;
	if (p == NULL || pay == NULL || p->tx_count >= ALPHY_Q) {
		return 0;
	}
	if (n > ALPHY_PAY) {
		n = ALPHY_PAY;
	}
	slot = p->txq[p->tx_head];
	alphy_fill(slot, 0, ALPHY_PAY);
	alphy_copy(slot, pay, n);
	p->tx_head = (p->tx_head + 1u) % ALPHY_Q;
	p->tx_count++;
	return 1;
}

unsigned alphy_recv(struct alphy *p, uint8_t *pay, unsigned cap)
{
	if (p == NULL || pay == NULL || p->rx_count == 0 || cap < ALPHY_PAY) {
		return 0;
	}
	alphy_copy(pay, p->rxq[p->rx_tail], ALPHY_PAY);
	p->rx_tail = (p->rx_tail + 1u) % ALPHY_Q;
	p->rx_count--;
	return ALPHY_PAY;
}

void alphy_gen(struct alphy *p, int16_t *stereo, uint32_t frames, int loopback)
{
	uint32_t i;
	if (p == NULL || stereo == NULL) {
		return;
	}
	for (i = 0; i < frames; i++) {
		int16_t l, r;
		alphy_tx_one(p, &l, &r);
		stereo[i * 2u] = l;
		stereo[i * 2u + 1u] = r;
		if (loopback) {
			alphy_rx_one(p, l, r);
		}
	}
}

void alphy_adc(struct alphy *p, const int16_t *stereo, uint32_t frames)
{
	uint32_t i;
	if (p == NULL || stereo == NULL) {
		return;
	}
	for (i = 0; i < frames; i++) {
		alphy_rx_one(p, stereo[i * 2u], stereo[i * 2u + 1u]);
	}
}

int alphy_locked(const struct alphy *p)
{
	return p && p->locked;
}

uint32_t alphy_tx_frames(const struct alphy *p)
{
	return p ? p->tx_frames : 0;
}

uint32_t alphy_rx_ok(const struct alphy *p)
{
	return p ? p->rx_ok : 0;
}

uint32_t alphy_rx_bad(const struct alphy *p)
{
	return p ? p->rx_bad : 0;
}

unsigned alphy_tx_space(const struct alphy *p)
{
	return p ? (ALPHY_Q - p->tx_count) : 0;
}

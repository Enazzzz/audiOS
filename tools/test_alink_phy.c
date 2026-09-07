#include "../common/alink_phy.h"

#include <stdio.h>
#include <string.h>
#include <stdint.h>

/**
 * Host-side proof of the stereo 48 kHz Pulse PHY.
 * Compile: cc -O2 -o tools/test_alink_phy tools/test_alink_phy.c common/alink_phy.c
 */

/** Fail the process with a message. */
static int fail(const char *msg)
{
	fprintf(stderr, "test_alink_phy: %s\n", msg);
	return 1;
}

/** Fill a 128-byte payload with a recognisable pattern plus a sequence number. */
static void make_pay(uint8_t *p, unsigned seq)
{
	unsigned i;
	memset(p, 0, ALPHY_PAY);
	p[0] = 0xA5;
	p[1] = 0x5A;
	p[2] = (uint8_t)seq;
	p[3] = (uint8_t)(seq >> 8);
	for (i = 4; i < 64u; i++) {
		p[i] = (uint8_t)(i * 17u + seq);
	}
	memcpy(p + 64, "audiOS Pulse 48k stereo", 23);
}

/** Round-trip `count` payloads through loopback PCM. */
static int roundtrip(int distort)
{
	struct alphy phy;
	uint8_t send[ALPHY_PAY];
	uint8_t got[ALPHY_PAY];
	int16_t pcm[48 * 2];
	unsigned seq, seen;
	uint32_t i;

	alphy_reset(&phy);
	seen = 0;
	for (seq = 0; seq < 200u; seq++) {
		make_pay(send, seq);
		if (!alphy_send(&phy, send, ALPHY_PAY)) {
			return fail("TX queue full before gen");
		}
		alphy_gen(&phy, pcm, ALPHY_FRAME_SAMP, 0);
		if (distort) {
			/*
			 * Residual DC from analog coupling. Line-out/line-in
			 * gain is ~1 on a short 3.5 mm lead; `audio gain`
			 * trims the rest. Do not scale the constellation here.
			 */
			for (i = 0; i < ALPHY_FRAME_SAMP; i++) {
				int32_t l = (int32_t)pcm[i * 2u] + 700;
				int32_t r = (int32_t)pcm[i * 2u + 1u] + 640;
				if (l > 32767) {
					l = 32767;
				}
				if (l < -32767) {
					l = -32767;
				}
				if (r > 32767) {
					r = 32767;
				}
				if (r < -32767) {
					r = -32767;
				}
				pcm[i * 2u] = (int16_t)l;
				pcm[i * 2u + 1u] = (int16_t)r;
			}
		}
		alphy_adc(&phy, pcm, ALPHY_FRAME_SAMP);
		while (alphy_recv(&phy, got, ALPHY_PAY) == ALPHY_PAY) {
			make_pay(send, seen);
			if (memcmp(got, send, ALPHY_PAY) != 0) {
				return fail(distort ? "distorted payload mismatch" : "payload mismatch");
			}
			seen++;
		}
	}
	if (seen < 190u) {
		fprintf(stderr, "test_alink_phy: only recovered %u/200 frames (ok=%u bad=%u)\n",
			seen, alphy_rx_ok(&phy), alphy_rx_bad(&phy));
		return 1;
	}
	return 0;
}

/**
 * 1.000 s of audio must carry ~1.024 Mbit of payload (128 bytes × 1000 frames).
 */
static int bitrate(void)
{
	struct alphy phy;
	int16_t pcm[480 * 2];
	uint8_t pay[ALPHY_PAY];
	uint8_t got[ALPHY_PAY];
	unsigned n, gen_frames, rec;
	uint32_t bits;

	alphy_reset(&phy);
	memset(pay, 0x3C, sizeof(pay));
	pay[0] = 0x01;
	rec = 0;
	for (gen_frames = 0; gen_frames < 1000u; gen_frames++) {
		pay[1] = (uint8_t)gen_frames;
		pay[2] = (uint8_t)(gen_frames >> 8);
		(void)alphy_send(&phy, pay, ALPHY_PAY);
		alphy_gen(&phy, pcm, ALPHY_FRAME_SAMP, 1);
		while (alphy_recv(&phy, got, ALPHY_PAY) == ALPHY_PAY) {
			rec++;
		}
	}
	bits = rec * ALPHY_PAY * 8u;
	if (bits < 900000u) {
		fprintf(stderr, "test_alink_phy: bitrate too low (%u bits in 1s, rec=%u)\n",
			bits, rec);
		return 1;
	}
	n = alphy_tx_frames(&phy);
	printf("phy ok  %u frames  %u payloads  %u bit/s  lock=%d  bad=%u\n",
		n, rec, bits, alphy_locked(&phy), alphy_rx_bad(&phy));
	return 0;
}

/** Two PHYs, full duplex, A→B and B→A in the same millisecond. */
static int duplex(void)
{
	struct alphy a, b;
	int16_t ab[ALPHY_FRAME_SAMP * 2];
	int16_t ba[ALPHY_FRAME_SAMP * 2];
	uint8_t pa[ALPHY_PAY], pb[ALPHY_PAY], ga[ALPHY_PAY], gb[ALPHY_PAY];
	unsigned i, got_a, got_b;

	alphy_reset(&a);
	alphy_reset(&b);
	got_a = got_b = 0;
	for (i = 0; i < 80u; i++) {
		memset(pa, (int)(0x10 + i), ALPHY_PAY);
		memset(pb, (int)(0x80 + i), ALPHY_PAY);
		pa[0] = (uint8_t)i;
		pb[0] = (uint8_t)i;
		(void)alphy_send(&a, pa, ALPHY_PAY);
		(void)alphy_send(&b, pb, ALPHY_PAY);
		alphy_gen(&a, ab, ALPHY_FRAME_SAMP, 0);
		alphy_gen(&b, ba, ALPHY_FRAME_SAMP, 0);
		alphy_adc(&b, ab, ALPHY_FRAME_SAMP);
		alphy_adc(&a, ba, ALPHY_FRAME_SAMP);
		if (alphy_recv(&a, ga, ALPHY_PAY) == ALPHY_PAY) {
			if (ga[0] != (uint8_t)got_a || ga[5] != (uint8_t)(0x80 + got_a)) {
				return fail("duplex A received the wrong stream");
			}
			got_a++;
		}
		if (alphy_recv(&b, gb, ALPHY_PAY) == ALPHY_PAY) {
			if (gb[0] != (uint8_t)got_b || gb[5] != (uint8_t)(0x10 + got_b)) {
				return fail("duplex B received the wrong stream");
			}
			got_b++;
		}
	}
	if (got_a < 70u || got_b < 70u) {
		fprintf(stderr, "test_alink_phy: duplex short A=%u B=%u\n", got_a, got_b);
		return 1;
	}
	return 0;
}

int main(void)
{
	if (ALPHY_PAY != 122u || ALPHY_RATE != 48000u) {
		return fail("unexpected PHY constants");
	}
	if (roundtrip(0)) {
		return 1;
	}
	if (roundtrip(1)) {
		return 1;
	}
	if (duplex()) {
		return 1;
	}
	if (bitrate()) {
		return 1;
	}
	return 0;
}

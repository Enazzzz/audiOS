#include "alink.h"
#include "audio.h"
#include "fat.h"
#include "fs.h"
#include "hda.h"
#include "kbd.h"
#include "klib.h"
#include "pit.h"
#include "shell.h"
#include "tty.h"
#include "version.h"

#include <stdint.h>

#define AL_SPS		20u	/* 48 kHz / 2400 baud */
#define AL_HALF		10u
#define AL_AMP		14000
#define AL_MAX_PAY	128u
#define AL_HDR		8u
#define AL_MAX_RAW	(AL_HDR + AL_MAX_PAY + 2u)
#define AL_COBS_MAX	(AL_MAX_RAW + 4u)
#define AL_TX_BITS	4096u
#define AL_RX_BYTES	192u
#define AL_CELLS	6144u

#define AL_VER		1u
#define AL_F_ACKREQ	0x01u
#define AL_F_ACK	0x02u

#define AL_HELLO	0x01u
#define AL_ACK		0x02u
#define AL_PING		0x03u
#define AL_PONG		0x04u
#define AL_TERM		0x10u
#define AL_CUR		0x12u
#define AL_KEY		0x20u
#define AL_CMD		0x21u
#define AL_FBEGIN	0x30u
#define AL_FDATA	0x31u
#define AL_FACK		0x32u
#define AL_FEND		0x33u
#define AL_FNAK		0x34u

#define AL_RETRY_MS	1500u
#define AL_RETRIES	8u
#define AL_SHARE_MS	250u

static int live;
static int loopback;
static int share;
static int view;
static int connected;
static char last_err[80];
static uint32_t tx_ok;
static uint32_t rx_ok;
static uint32_t rx_bad;
static uint32_t rx_drops;

static uint8_t tx_bits[AL_TX_BITS];
static unsigned tx_n;
static unsigned tx_i;
static unsigned tx_phase;

static uint8_t rx_bytes[AL_RX_BYTES];
static unsigned rx_n;
static int rx_in_frame;
static uint8_t rx_acc;
static unsigned rx_bbit;
static int32_t rx_s0;
static int32_t rx_s1;
static unsigned rx_sub;
static unsigned rx_idle;
static uint16_t last_rx_seq = 0xFFFFu;

static uint8_t wait_raw[AL_MAX_RAW];
static unsigned wait_len;
static int wait_busy;
static unsigned wait_tries;
static uint64_t wait_until;
static uint16_t tx_seq;
static uint16_t rx_ack_seq;

static uint8_t last_ch[AL_CELLS];
static uint8_t last_pal[AL_CELLS];
static int snap_ok;
static uint64_t next_share;

static int pend_key;
static int pend_key_n;
static char pend_cmd[120];
static int pend_cmd_ready;
static int pend_pong;
static char peer_name[48];

static char file_path[FAT_PATH_MAX];
static uint32_t file_size;
static uint32_t file_got;
static int file_rx;
static uint8_t file_pend[68];
static unsigned file_pend_n;
static int file_pend_ready;
static int file_pend_new;

static void alink_dispatch(uint8_t type, const uint8_t *pay, unsigned n);

/** CRC-16-CCITT (0x1021, init 0xFFFF) over `n` bytes. */
static uint16_t crc16(const uint8_t *p, unsigned n)
{
	uint16_t c = 0xFFFFu;
	unsigned i, b;
	for (i = 0; i < n; i++) {
		c ^= (uint16_t)p[i] << 8;
		for (b = 0; b < 8u; b++) {
			c = (c & 0x8000u) ? (uint16_t)((c << 1) ^ 0x1021u) : (uint16_t)(c << 1);
		}
	}
	return c;
}

/** COBS encode `n` bytes so the output contains no 0x00. */
static unsigned cobs_encode(const uint8_t *in, unsigned n, uint8_t *out)
{
	unsigned i;
	uint8_t *codep = out;
	uint8_t *dst = out + 1;
	uint8_t code = 1;
	for (i = 0; i < n; i++) {
		if (in[i] == 0) {
			*codep = code;
			codep = dst++;
			code = 1;
		} else {
			*dst++ = in[i];
			code++;
			if (code == 0xFFu) {
				*codep = code;
				codep = dst++;
				code = 1;
			}
		}
	}
	*codep = code;
	return (unsigned)(dst - out);
}

/** Inverse of `cobs_encode`. Returns 0 on a truncated or illegal stream. */
static unsigned cobs_decode(const uint8_t *in, unsigned n, uint8_t *out, unsigned cap)
{
	unsigned i = 0, o = 0;
	while (i < n) {
		unsigned code = in[i++];
		unsigned k;
		if (code == 0) {
			return 0;
		}
		for (k = 1; k < code; k++) {
			if (i >= n || o >= cap) {
				return 0;
			}
			out[o++] = in[i++];
		}
		if (code != 0xFFu && i < n) {
			if (o >= cap) {
				return 0;
			}
			out[o++] = 0;
		}
	}
	return o;
}

static void tx_push_bit(int bit)
{
	if (tx_n < AL_TX_BITS) {
		tx_bits[tx_n++] = bit ? 1 : 0;
	}
}

static void tx_push_byte(uint8_t b)
{
	unsigned k;
	for (k = 0; k < 8u; k++) {
		tx_push_bit((b >> (7u - k)) & 1u);
	}
}

/** Preamble bits, 0x00 delimiter, COBS body, 0x00. */
static void tx_queue_cobs(const uint8_t *cobs, unsigned n)
{
	unsigned i, p;
	for (p = 0; p < 24u; p++) {
		tx_push_bit(p & 1u);
	}
	tx_push_byte(0x00);
	for (i = 0; i < n; i++) {
		tx_push_byte(cobs[i]);
	}
	tx_push_byte(0x00);
}

static unsigned pack_frame(uint8_t type, uint8_t flags, uint16_t seq, uint16_t ack,
	const uint8_t *pay, unsigned payn, uint8_t *raw)
{
	unsigned i;
	uint16_t crc;
	if (payn > AL_MAX_PAY) {
		payn = AL_MAX_PAY;
	}
	raw[0] = AL_VER;
	raw[1] = type;
	raw[2] = flags;
	raw[3] = (uint8_t)seq;
	raw[4] = (uint8_t)(seq >> 8);
	raw[5] = (uint8_t)ack;
	raw[6] = (uint8_t)(ack >> 8);
	raw[7] = (uint8_t)payn;
	for (i = 0; i < payn; i++) {
		raw[AL_HDR + i] = pay[i];
	}
	crc = crc16(raw, AL_HDR + payn);
	raw[AL_HDR + payn] = (uint8_t)crc;
	raw[AL_HDR + payn + 1u] = (uint8_t)(crc >> 8);
	return AL_HDR + payn + 2u;
}

static int send_now(uint8_t type, uint8_t flags, const uint8_t *pay, unsigned payn, int wait_ack)
{
	uint8_t raw[AL_MAX_RAW];
	uint8_t cobs[AL_COBS_MAX];
	unsigned n, cn;
	uint16_t seq = tx_seq;
	if (wait_busy && wait_ack) {
		return 0;
	}
	if (type != AL_ACK) {
		tx_seq = (uint16_t)(tx_seq + 1u);
	} else {
		seq = 0;
	}
	n = pack_frame(type, flags, seq, rx_ack_seq, pay, payn, raw);
	cn = cobs_encode(raw, n, cobs);
	if (tx_i >= tx_n) {
		tx_i = 0;
		tx_n = 0;
		tx_phase = 0;
	}
	tx_queue_cobs(cobs, cn);
	tx_ok++;
	if (wait_ack) {
		memcpy(wait_raw, raw, n);
		wait_len = n;
		wait_busy = 1;
		wait_tries = 1;
		wait_until = pit_ticks() + AL_RETRY_MS;
	}
	return 1;
}

static void send_ack(uint16_t seq)
{
	uint8_t p[2];
	p[0] = (uint8_t)seq;
	p[1] = (uint8_t)(seq >> 8);
	send_now(AL_ACK, AL_F_ACK, p, 2, 0);
}

/** One PCM sample of IEEE-style Manchester for `bit` at `phase` in 0..SPS-1. */
static int16_t man_sample(int bit, unsigned phase)
{
	int high_first = bit ? 1 : 0;
	int first = (phase < AL_HALF);
	int pos = first ? high_first : !high_first;
	return pos ? (int16_t)AL_AMP : (int16_t)(-AL_AMP);
}

static void rx_bit(int bit)
{
	uint8_t raw[AL_MAX_RAW];
	unsigned n, payn;
	uint16_t crc, got, seq;
	uint8_t type, flags;

	rx_acc = (uint8_t)((rx_acc << 1) | (bit ? 1u : 0u));
	rx_bbit++;
	if (rx_bbit < 8u) {
		return;
	}
	rx_bbit = 0;
	if (!rx_in_frame) {
		if (rx_acc == 0x00) {
			rx_in_frame = 1;
			rx_n = 0;
		}
		return;
	}
	if (rx_acc != 0x00) {
		if (rx_n < AL_RX_BYTES) {
			rx_bytes[rx_n++] = rx_acc;
		} else {
			rx_in_frame = 0;
			rx_drops++;
		}
		return;
	}
	rx_in_frame = 0;
	if (rx_n < 2u) {
		return;
	}
	n = cobs_decode(rx_bytes, rx_n, raw, sizeof(raw));
	rx_n = 0;
	if (n < AL_HDR + 2u) {
		rx_bad++;
		return;
	}
	payn = raw[7];
	if (payn > AL_MAX_PAY || n < AL_HDR + payn + 2u) {
		rx_bad++;
		return;
	}
	crc = crc16(raw, AL_HDR + payn);
	got = (uint16_t)raw[AL_HDR + payn] | ((uint16_t)raw[AL_HDR + payn + 1u] << 8);
	if (crc != got || raw[0] != AL_VER) {
		rx_bad++;
		return;
	}
	type = raw[1];
	flags = raw[2];
	seq = (uint16_t)raw[3] | ((uint16_t)raw[4] << 8);
	rx_ok++;
	if (flags & AL_F_ACKREQ) {
		send_ack(seq);
	}
	if (type == AL_ACK && payn >= 2u && wait_busy) {
		uint16_t a = (uint16_t)raw[AL_HDR] | ((uint16_t)raw[AL_HDR + 1u] << 8);
		uint16_t wseq = (uint16_t)wait_raw[3] | ((uint16_t)wait_raw[4] << 8);
		if (a == wseq) {
			wait_busy = 0;
		}
		return;
	}
	if (seq == last_rx_seq && type != AL_PONG && type != AL_HELLO) {
		return;
	}
	last_rx_seq = seq;
	rx_ack_seq = seq;
	alink_dispatch(type, raw + AL_HDR, payn);
}

static void rx_sample(int16_t s)
{
	int32_t mag = s < 0 ? -(int32_t)s : (int32_t)s;
	if (mag < 2000) {
		if (rx_idle < 64u) {
			rx_idle++;
		}
		if (rx_idle >= (AL_SPS * 2u) && !rx_in_frame) {
			rx_sub = 0;
			rx_s0 = 0;
			rx_s1 = 0;
			rx_bbit = 0;
			rx_acc = 0;
			return;
		}
	} else {
		rx_idle = 0;
	}
	if (rx_sub < AL_HALF) {
		rx_s0 += s;
	} else {
		rx_s1 += s;
	}
	rx_sub++;
	if (rx_sub < AL_SPS) {
		return;
	}
	rx_bit(rx_s0 > rx_s1);
	rx_s0 = 0;
	rx_s1 = 0;
	rx_sub = 0;
}

void alink_rx_pcm(const int16_t *stereo, uint32_t frames)
{
	uint32_t i;
	if (!live || loopback || stereo == NULL) {
		return;
	}
	for (i = 0; i < frames; i++) {
		int32_t m = ((int32_t)stereo[i * 2u] + (int32_t)stereo[i * 2u + 1u]) / 2;
		rx_sample((int16_t)m);
	}
}

void alink_fill(int16_t *dst, uint32_t frames)
{
	uint32_t i;
	int16_t s;
	for (i = 0; i < frames; i++) {
		s = 0;
		if (tx_i < tx_n) {
			s = man_sample((int)tx_bits[tx_i], tx_phase);
			tx_phase++;
			if (tx_phase >= AL_SPS) {
				tx_phase = 0;
				tx_i++;
			}
		}
		dst[i * 2u] = s;
		dst[i * 2u + 1u] = s;
		if (loopback) {
			rx_sample(s);
		}
	}
}

static uint8_t pal_of(uint32_t rgb)
{
	if (rgb == TTY_COL_DIM) {
		return 1;
	}
	if (rgb == TTY_COL_ACCENT) {
		return 2;
	}
	if (rgb == TTY_COL_AUDIO) {
		return 3;
	}
	if (rgb == TTY_COL_ERR) {
		return 4;
	}
	if (rgb == TTY_COL_SEL_FG) {
		return 5;
	}
	return 0;
}

static uint32_t rgb_of(uint8_t pal)
{
	switch (pal) {
	case 1:
		return TTY_COL_DIM;
	case 2:
		return TTY_COL_ACCENT;
	case 3:
		return TTY_COL_AUDIO;
	case 4:
		return TTY_COL_ERR;
	case 5:
		return TTY_COL_SEL_FG;
	default:
		return TTY_COL_FG;
	}
}

static void share_tick(void)
{
	unsigned cols = tty_cols();
	unsigned rows = tty_rows();
	unsigned r, c;
	uint8_t pay[AL_MAX_PAY];
	unsigned o = 0;
	if (!share || !live || wait_busy) {
		return;
	}
	if (pit_ticks() < next_share) {
		return;
	}
	next_share = pit_ticks() + AL_SHARE_MS;
	if (!snap_ok) {
		memset(last_ch, 0, sizeof(last_ch));
		memset(last_pal, 0xFF, sizeof(last_pal));
		snap_ok = 1;
	}
	for (r = 0; r < rows; r++) {
		for (c = 0; c < cols; c++) {
			unsigned char ch = ' ';
			uint32_t rgb = TTY_COL_FG;
			uint8_t pal;
			unsigned idx = r * cols + c;
			if (idx >= AL_CELLS) {
				break;
			}
			tty_cell_at(c, r, &ch, &rgb);
			pal = pal_of(rgb);
			if (last_ch[idx] == (uint8_t)ch && last_pal[idx] == pal) {
				continue;
			}
			if (o + 4u > AL_MAX_PAY) {
				send_now(AL_TERM, AL_F_ACKREQ, pay, o, 1);
				return;
			}
			pay[o++] = (uint8_t)r;
			pay[o++] = (uint8_t)c;
			pay[o++] = (uint8_t)ch;
			pay[o++] = pal;
			last_ch[idx] = (uint8_t)ch;
			last_pal[idx] = pal;
		}
	}
	if (o > 0) {
		send_now(AL_TERM, AL_F_ACKREQ, pay, o, 1);
	} else {
		uint8_t cur[2];
		cur[0] = (uint8_t)tty_cursor_col();
		cur[1] = (uint8_t)tty_cursor_row();
		send_now(AL_CUR, 0, cur, 2, 0);
	}
}

static void apply_term(const uint8_t *pay, unsigned n)
{
	unsigned i;
	if (!view) {
		return;
	}
	for (i = 0; i + 4u <= n; i += 4u) {
		tty_put_xy(pay[i + 1u], pay[i], (char)pay[i + 2u], rgb_of(pay[i + 3u]));
	}
}

static void apply_cur(const uint8_t *pay, unsigned n)
{
	if (!view || n < 2) {
		return;
	}
	tty_set_cursor(pay[0], pay[1]);
}

static void file_begin(const uint8_t *pay, unsigned n)
{
	uint32_t size;
	unsigned i;
	char name[FAT_NAME_MAX];
	struct fat_info inf;
	uint8_t ack[4];
	const char *leaf;
	if (n < 5) {
		return;
	}
	size = (uint32_t)pay[0] | ((uint32_t)pay[1] << 8)
		| ((uint32_t)pay[2] << 16) | ((uint32_t)pay[3] << 24);
	n -= 4;
	if (n >= sizeof(name)) {
		n = sizeof(name) - 1u;
	}
	for (i = 0; i < n; i++) {
		name[i] = (char)pay[4 + i];
	}
	name[n] = '\0';
	leaf = name;
	if (fat_vol_ready(FAT_VOL_USR)) {
		ksnprintf(file_path, sizeof(file_path), "D:/%s", leaf);
		fat_select(FAT_VOL_USR);
	} else {
		ksnprintf(file_path, sizeof(file_path), "C:/%s", leaf);
		fat_select(FAT_VOL_SYS);
	}
	file_size = size;
	file_got = 0;
	file_rx = 1;
	if (fat_stat(leaf[0] == '/' ? leaf : leaf, &inf) && inf.kind == FAT_FILE) {
		file_got = inf.size <= file_size ? inf.size : 0;
	}
	{
		char slash[FAT_PATH_MAX];
		ksnprintf(slash, sizeof(slash), "/%s", leaf);
		if (fat_stat(slash, &inf) && inf.kind == FAT_FILE) {
			file_got = inf.size <= file_size ? inf.size : 0;
		}
	}
	ack[0] = (uint8_t)file_got;
	ack[1] = (uint8_t)(file_got >> 8);
	ack[2] = (uint8_t)(file_got >> 16);
	ack[3] = (uint8_t)(file_got >> 24);
	send_now(AL_FACK, 0, ack, 4, 0);
}

static void file_data(const uint8_t *pay, unsigned n)
{
	uint32_t off;
	if (!file_rx || n < 4) {
		return;
	}
	off = (uint32_t)pay[0] | ((uint32_t)pay[1] << 8)
		| ((uint32_t)pay[2] << 16) | ((uint32_t)pay[3] << 24);
	pay += 4;
	n -= 4;
	if (off != file_got) {
		uint8_t nak[4];
		nak[0] = (uint8_t)file_got;
		nak[1] = (uint8_t)(file_got >> 8);
		nak[2] = (uint8_t)(file_got >> 16);
		nak[3] = (uint8_t)(file_got >> 24);
		send_now(AL_FNAK, 0, nak, 4, 0);
		return;
	}
	if (file_pend_ready) {
		return;
	}
	if (n > sizeof(file_pend)) {
		n = sizeof(file_pend);
	}
	memcpy(file_pend, pay, n);
	file_pend_n = n;
	file_pend_new = (off == 0);
	file_pend_ready = 1;
	file_got += n;
}

static void hello_in(const uint8_t *pay, unsigned n)
{
	unsigned i;
	connected = 1;
	if (n >= sizeof(peer_name)) {
		n = sizeof(peer_name) - 1u;
	}
	for (i = 0; i < n; i++) {
		peer_name[i] = (char)pay[i];
	}
	peer_name[n] = '\0';
}

static void alink_dispatch(uint8_t type, const uint8_t *pay, unsigned n)
{
	switch (type) {
	case AL_HELLO:
		hello_in(pay, n);
		break;
	case AL_PING:
		send_now(AL_PONG, 0, pay, n, 0);
		break;
	case AL_PONG:
		pend_pong = 1;
		break;
	case AL_TERM:
		apply_term(pay, n);
		break;
	case AL_CUR:
		apply_cur(pay, n);
		break;
	case AL_KEY:
		if (n >= 2 && pend_key_n == 0) {
			pend_key = (int)((uint16_t)pay[0] | ((uint16_t)pay[1] << 8));
			pend_key_n = 1;
		}
		break;
	case AL_CMD:
		if (n > 0 && !pend_cmd_ready) {
			if (n >= sizeof(pend_cmd)) {
				n = sizeof(pend_cmd) - 1u;
			}
			memcpy(pend_cmd, pay, n);
			pend_cmd[n] = '\0';
			pend_cmd_ready = 1;
		}
		break;
	case AL_FBEGIN:
		file_begin(pay, n);
		break;
	case AL_FDATA:
		file_data(pay, n);
		break;
	case AL_FEND:
		file_rx = 0;
		break;
	default:
		break;
	}
}

static int wait_clear(uint32_t ms)
{
	uint64_t t0 = pit_ticks();
	while (wait_busy && pit_ticks() - t0 < ms) {
		audio_service();
	}
	return !wait_busy;
}

static void send_hello(void)
{
	char msg[64];
	ksnprintf(msg, sizeof(msg), "%s %s %s", AUDIOS_NAME, AUDIOS_VERSION_STRING, AUDIOS_BOARD);
	send_now(AL_HELLO, 0, (const uint8_t *)msg, (unsigned)strlen(msg), 0);
}

static int alink_start(int loop)
{
	loopback = loop ? 1 : 0;
	live = 1;
	rx_n = 0;
	rx_in_frame = 0;
	rx_bbit = 0;
	rx_sub = 0;
	rx_s0 = 0;
	rx_s1 = 0;
	rx_idle = 0;
	tx_n = 0;
	tx_i = 0;
	tx_phase = 0;
	wait_busy = 0;
	connected = 0;
	peer_name[0] = '\0';
	last_err[0] = '\0';
	if (!loopback && hda_has_capture()) {
		hda_select_input(0);
		hda_cap_hook(alink_rx_pcm);
	} else {
		hda_cap_hook(0);
	}
	/* Queue the HELLO before DMA prefill so the first periods are not silence. */
	send_hello();
	if (!audio_dma_hold(1)) {
		live = 0;
		ksnprintf(last_err, sizeof(last_err), "%s", "DAC would not start");
		return 0;
	}
	return 1;
}

static void alink_stop(void)
{
	live = 0;
	loopback = 0;
	share = 0;
	view = 0;
	wait_busy = 0;
	hda_cap_hook(0);
	audio_dma_hold(0);
}

void alink_init(void)
{
	live = 0;
	loopback = 0;
}

int alink_active(void)
{
	return live;
}

int alink_viewing(void)
{
	return view;
}

void alink_exit_view(void)
{
	view = 0;
}

int alink_send_key(int key)
{
	uint8_t p[2];
	if (!live) {
		return 0;
	}
	p[0] = (uint8_t)key;
	p[1] = (uint8_t)((unsigned)key >> 8);
	return send_now(AL_KEY, 0, p, 2, 0);
}

void alink_service(void)
{
	if (!live) {
		return;
	}
	if (wait_busy && pit_ticks() >= wait_until) {
		if (wait_tries >= AL_RETRIES) {
			wait_busy = 0;
			ksnprintf(last_err, sizeof(last_err), "%s", "retry exhausted");
		} else {
			uint8_t cobs[AL_COBS_MAX];
			unsigned cn = cobs_encode(wait_raw, wait_len, cobs);
			if (tx_i >= tx_n) {
				tx_i = 0;
				tx_n = 0;
				tx_phase = 0;
			}
			tx_queue_cobs(cobs, cn);
			wait_tries++;
			wait_until = pit_ticks() + AL_RETRY_MS;
		}
	}
	share_tick();
}

void alink_poll(void)
{
	if (pend_key_n) {
		kbd_inject(pend_key);
		pend_key_n = 0;
	}
	if (pend_cmd_ready) {
		tty_set_color(TTY_COL_ACCENT);
		tty_printf("[link] %s\n", pend_cmd);
		tty_set_color(TTY_COL_FG);
		shell_run_line(pend_cmd);
		pend_cmd_ready = 0;
	}
	if (file_pend_ready) {
		if (file_pend_new) {
			(void)fs_write_file(file_path, file_pend, file_pend_n);
		} else {
			(void)fs_append(file_path, file_pend, file_pend_n);
		}
		file_pend_ready = 0;
	}
}

/** Encode one PING into PCM and decode it with no DAC (CI / `link test`). */
static int phy_selftest(void)
{
	uint8_t raw[AL_MAX_RAW];
	uint8_t cobs[AL_COBS_MAX];
	uint8_t pay[8] = { 'a', 'u', 'd', 'i', 'O', 'S', '!' };
	int16_t pcm[4096];
	unsigned n, cn, ns, i;
	tx_n = tx_i = tx_phase = 0;
	rx_n = 0;
	rx_in_frame = 0;
	rx_bbit = 0;
	rx_sub = 0;
	rx_s0 = rx_s1 = 0;
	rx_idle = 0;
	rx_ok = 0;
	rx_bad = 0;
	pend_pong = 0;
	n = pack_frame(AL_PING, 0, 1, 0, pay, 7, raw);
	cn = cobs_encode(raw, n, cobs);
	tx_queue_cobs(cobs, cn);
	ns = 0;
	while (tx_i < tx_n && ns < 4095u) {
		pcm[ns++] = man_sample((int)tx_bits[tx_i], tx_phase);
		tx_phase++;
		if (tx_phase >= AL_SPS) {
			tx_phase = 0;
			tx_i++;
		}
	}
	for (i = 0; i < ns; i++) {
		rx_sample(pcm[i]);
	}
	return pend_pong || rx_ok > 0;
}

static void cmd_status(void)
{
	tty_puts("Audio Link  48 kHz 16-bit L+R  Manchester 2400\n");
	tty_printf("  %s  %s  %s\n",
		live ? "up" : "down",
		loopback ? "loopback" : "line",
		connected ? "peer" : "no peer");
	if (peer_name[0]) {
		tty_printf("  peer: %s\n", peer_name);
	}
	tty_printf("  tx %u  rx %u  bad %u  drop %u\n", tx_ok, rx_ok, rx_bad, rx_drops);
	if (share) {
		tty_puts("  sharing this terminal\n");
	}
	if (view) {
		tty_puts("  viewing peer (Ctrl-X returns)\n");
	}
	if (file_rx) {
		tty_printf("  recv %s  %u/%u\n", file_path, file_got, file_size);
	}
	if (last_err[0]) {
		tty_set_color(TTY_COL_ERR);
		tty_printf("  last: %s\n", last_err);
		tty_set_color(TTY_COL_FG);
	}
	tty_puts("  cable: line-out -> peer line-in, both ways.\n");
}

static int cmd_ping(void)
{
	uint8_t token = 0x5A;
	uint64_t t0;
	int16_t tmp[256];
	unsigned i;
	if (!live && !alink_start(loopback)) {
		return 0;
	}
	pend_pong = 0;
	send_now(AL_PING, 0, &token, 1, 0);
	if (loopback) {
		/*
		 * Drain TX in software. Do not call audio_service here: that
		 * would run a second alink_fill on the DAC path and steal bits.
		 */
		for (i = 0; i < 80u && !pend_pong; i++) {
			alink_fill(tmp, 256);
		}
		if (pend_pong) {
			tty_puts("pong\n");
			return 1;
		}
		tty_set_color(TTY_COL_ERR);
		tty_puts("ping timeout\n");
		tty_set_color(TTY_COL_FG);
		return 0;
	}
	t0 = pit_ticks();
	while (pit_ticks() - t0 < 3000u) {
		audio_service();
		if (pend_pong) {
			tty_puts("pong\n");
			return 1;
		}
	}
	tty_set_color(TTY_COL_ERR);
	tty_puts("ping timeout\n");
	tty_set_color(TTY_COL_FG);
	return 0;
}

static int cmd_send(const char *path)
{
	uint8_t begin[4 + 48];
	const char *base;
	unsigned i, nl;
	uint32_t off = 0;
	uint8_t chunk[4 + 64];
	if (!fs_ready()) {
		tty_puts("no filesystem\n");
		return 0;
	}
	if (!live && !alink_start(loopback)) {
		return 0;
	}
	base = path;
	for (i = 0; path[i]; i++) {
		if (path[i] == '/' || path[i] == '\\') {
			base = path + i + 1u;
		}
	}
	nl = (unsigned)strlen(base);
	if (nl > 40u) {
		nl = 40u;
	}
	memset(begin, 0xFF, 4);
	memcpy(begin + 4, base, nl);
	send_now(AL_FBEGIN, AL_F_ACKREQ, begin, 4 + nl, 1);
	if (!wait_clear(4000)) {
		tty_set_color(TTY_COL_ERR);
		tty_puts("file begin timeout\n");
		tty_set_color(TTY_COL_FG);
		return 0;
	}
	for (;;) {
		uint32_t got = 0;
		if (!fs_read_at(path, off, chunk + 4, 64, &got) || got == 0) {
			break;
		}
		chunk[0] = (uint8_t)off;
		chunk[1] = (uint8_t)(off >> 8);
		chunk[2] = (uint8_t)(off >> 16);
		chunk[3] = (uint8_t)(off >> 24);
		send_now(AL_FDATA, AL_F_ACKREQ, chunk, 4 + (unsigned)got, 1);
		if (!wait_clear(4000)) {
			tty_set_color(TTY_COL_ERR);
			tty_puts("file chunk timeout\n");
			tty_set_color(TTY_COL_FG);
			return 0;
		}
		off += got;
		tty_printf("\r  %u bytes", off);
	}
	chunk[0] = (uint8_t)off;
	chunk[1] = (uint8_t)(off >> 8);
	chunk[2] = (uint8_t)(off >> 16);
	chunk[3] = (uint8_t)(off >> 24);
	send_now(AL_FEND, AL_F_ACKREQ, chunk, 4, 1);
	wait_clear(4000);
	tty_printf("\nsent %u bytes (%s)\n", off, path);
	return 1;
}

static int cmd_test(void)
{
	if (!phy_selftest()) {
		tty_set_color(TTY_COL_ERR);
		tty_puts("link test: PHY roundtrip failed\n");
		tty_set_color(TTY_COL_FG);
		return 0;
	}
	if (!alink_start(1)) {
		tty_set_color(TTY_COL_ERR);
		tty_printf("link test: %s\n", last_err[0] ? last_err : "start failed");
		tty_set_color(TTY_COL_FG);
		return 0;
	}
	if (!cmd_ping()) {
		alink_stop();
		return 0;
	}
	alink_stop();
	tty_set_color(TTY_COL_AUDIO);
	tty_puts("link test ok\n");
	tty_set_color(TTY_COL_FG);
	return 1;
}

void alink_cmd(int argc, char **argv)
{
	const char *sub = (argc > 1) ? argv[1] : "";
	if (sub[0] == '\0' || strcmp(sub, "status") == 0) {
		cmd_status();
		return;
	}
	if (strcmp(sub, "on") == 0 || strcmp(sub, "start") == 0) {
		if (alink_start(0)) {
			tty_puts("link up (line-out / line-in). Unplug speakers.\n");
		} else {
			tty_set_color(TTY_COL_ERR);
			tty_printf("link: %s\n", last_err);
			tty_set_color(TTY_COL_FG);
		}
		return;
	}
	if (strcmp(sub, "off") == 0 || strcmp(sub, "stop") == 0) {
		alink_stop();
		tty_puts("link down\n");
		return;
	}
	if (strcmp(sub, "loop") == 0) {
		if (alink_start(1)) {
			tty_puts("link loopback (TX=RX, no cable)\n");
		} else {
			tty_set_color(TTY_COL_ERR);
			tty_printf("link: %s\n", last_err);
			tty_set_color(TTY_COL_FG);
		}
		return;
	}
	if (strcmp(sub, "ping") == 0) {
		cmd_ping();
		return;
	}
	if (strcmp(sub, "share") == 0) {
		if (!live && !alink_start(loopback)) {
			return;
		}
		share = 1;
		snap_ok = 0;
		tty_puts("sharing this terminal over the link\n");
		return;
	}
	if (strcmp(sub, "view") == 0) {
		if (!live && !alink_start(loopback)) {
			return;
		}
		view = 1;
		tty_clear();
		tty_puts("link view — Ctrl-X returns to local\n");
		return;
	}
	if (strcmp(sub, "local") == 0) {
		view = 0;
		tty_puts("local terminal\n");
		return;
	}
	if (strcmp(sub, "cmd") == 0) {
		char line[120];
		int i;
		unsigned o = 0;
		if (argc < 3) {
			tty_puts("link cmd <command...>\n");
			return;
		}
		if (!live && !alink_start(loopback)) {
			return;
		}
		line[0] = '\0';
		for (i = 2; i < argc; i++) {
			unsigned n = (unsigned)strlen(argv[i]);
			if (o && o + 1u < sizeof(line)) {
				line[o++] = ' ';
			}
			if (o + n >= sizeof(line)) {
				n = sizeof(line) - 1u - o;
			}
			memcpy(line + o, argv[i], n);
			o += n;
			line[o] = '\0';
		}
		send_now(AL_CMD, AL_F_ACKREQ, (const uint8_t *)line, o, 1);
		tty_printf("sent command (%u bytes)\n", o);
		return;
	}
	if (strcmp(sub, "send") == 0) {
		if (argc < 3) {
			tty_puts("link send <file>\n");
			return;
		}
		cmd_send(argv[2]);
		return;
	}
	if (strcmp(sub, "test") == 0) {
		cmd_test();
		return;
	}
	tty_set_color(TTY_COL_ERR);
	tty_puts("link [status|on|off|loop|ping|share|view|cmd|send|test]\n");
	tty_set_color(TTY_COL_FG);
}

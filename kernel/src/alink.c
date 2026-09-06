#include "alink.h"
#include "alink_phy.h"
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

/*
 * MAC on top of audiOS Pulse (48 kHz stereo 12-bit PAM, ~1 Mbit/s).
 * Sliding window of 16 × 1 ms frames. FX (this kernel) defaults to master;
 * the A7V333 32-bit kernel defaults to slave.
 */

#define AL_HDR		9u
#define AL_MAX_PAY	(ALPHY_PAY - AL_HDR)
#define AL_WIN		16u
#define AL_APPQ		32u
#define AL_CELLS	6144u

#define AL_HELLO	0x01u
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

#define AL_RETRY_MS	40u
#define AL_RETRIES	12u
#define AL_SHARE_MS	16u

#ifndef AUDIOS_LINK_SLAVE
#define AUDIOS_LINK_SLAVE	0
#endif

static struct alphy phy;
static int live;
static int loopback;
static int share;
static int view;
static int connected;
static int is_master = !AUDIOS_LINK_SLAVE;
static char last_err[80];
static uint32_t tx_ok;
static uint32_t rx_ok;

static uint8_t app_type[AL_APPQ];
static uint8_t app_pay[AL_APPQ][AL_MAX_PAY];
static unsigned app_n[AL_APPQ];
static unsigned app_head, app_tail, app_count;

static uint8_t win_raw[AL_WIN][ALPHY_PAY];
static uint8_t win_busy[AL_WIN];
static uint8_t win_tries[AL_WIN];
static uint64_t win_t[AL_WIN];
static uint16_t tx_seq;
static uint16_t tx_unacked;

static uint16_t rx_next;
static uint16_t last_rx_seq = 0xFFFFu;

static uint8_t last_ch[AL_CELLS];
static uint8_t last_pal[AL_CELLS];
static int snap_ok;
static uint64_t next_share;
static uint64_t next_hello;

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
static uint8_t file_pend[120];
static unsigned file_pend_n;
static int file_pend_ready;
static int file_pend_new;

static void alink_dispatch(uint8_t type, const uint8_t *pay, unsigned n);
static void alink_pump(void);

/** Pack a MAC header + payload into one PHY frame (zero-padded). */
static void mac_pack(uint8_t type, uint16_t seq, const uint8_t *pay, unsigned payn, uint8_t *out)
{
	unsigned i;
	if (payn > AL_MAX_PAY) {
		payn = AL_MAX_PAY;
	}
	memset(out, 0, ALPHY_PAY);
	out[0] = type;
	out[1] = is_master ? 0x01u : 0x02u;
	out[2] = (uint8_t)seq;
	out[3] = (uint8_t)(seq >> 8);
	out[4] = (uint8_t)rx_next;
	out[5] = (uint8_t)(rx_next >> 8);
	out[6] = (uint8_t)tx_unacked;
	out[7] = (uint8_t)(tx_unacked >> 8);
	out[8] = (uint8_t)payn;
	for (i = 0; i < payn; i++) {
		out[AL_HDR + i] = pay[i];
	}
}

/** Enqueue an application message (HELLO/PING/TERM/…). Drops if full. */
static int app_push(uint8_t type, const uint8_t *pay, unsigned n)
{
	if (app_count >= AL_APPQ) {
		return 0;
	}
	if (n > AL_MAX_PAY) {
		n = AL_MAX_PAY;
	}
	app_type[app_head] = type;
	app_n[app_head] = n;
	if (pay && n) {
		memcpy(app_pay[app_head], pay, n);
	}
	app_head = (app_head + 1u) % AL_APPQ;
	app_count++;
	return 1;
}

/** True if `a` is before `b` in 16-bit sequence space. */
static int seq_before(uint16_t a, uint16_t b)
{
	return (int16_t)(a - b) < 0;
}

/** Mark window slots at or before `ack` as done. */
static void win_ack(uint16_t ack)
{
	unsigned i;
	for (i = 0; i < AL_WIN; i++) {
		if (!win_busy[i]) {
			continue;
		}
		uint16_t s = (uint16_t)win_raw[i][2] | ((uint16_t)win_raw[i][3] << 8);
		if (!seq_before(ack, (uint16_t)(s + 1u))) {
			win_busy[i] = 0;
		}
	}
	while (seq_before(tx_unacked, tx_seq) && !win_busy[tx_unacked % AL_WIN]) {
		tx_unacked = (uint16_t)(tx_unacked + 1u);
	}
}

/** Outstanding frames in the sliding window. */
static unsigned win_inflight(void)
{
	return (unsigned)(uint16_t)(tx_seq - tx_unacked);
}

/** Queue one windowed MAC frame onto the PHY. */
static int win_send(uint8_t type, const uint8_t *pay, unsigned n, int need_ack)
{
	unsigned slot;
	uint16_t seq;
	if (need_ack && win_inflight() >= AL_WIN) {
		return 0;
	}
	seq = need_ack ? tx_seq : 0;
	slot = seq % AL_WIN;
	mac_pack(type, seq, pay, n, win_raw[slot]);
	if (!alphy_send(&phy, win_raw[slot], ALPHY_PAY)) {
		return 0;
	}
	if (need_ack && type != 0) {
		win_busy[slot] = 1;
		win_tries[slot] = 1;
		win_t[slot] = pit_ticks();
		tx_seq = (uint16_t)(tx_seq + 1u);
	}
	tx_ok++;
	return 1;
}

/** Push idle ACK / app / retransmit so the PHY never goes silent. */
static void alink_pump(void)
{
	unsigned i;
	uint64_t now;
	if (!live) {
		return;
	}
	now = pit_ticks();
	for (i = 0; i < AL_WIN; i++) {
		if (!win_busy[i]) {
			continue;
		}
		if (now - win_t[i] < AL_RETRY_MS) {
			continue;
		}
		if (win_tries[i] >= AL_RETRIES) {
			win_busy[i] = 0;
			ksnprintf(last_err, sizeof(last_err), "%s", "retry exhausted");
			continue;
		}
		if (alphy_tx_space(&phy) == 0) {
			break;
		}
		(void)alphy_send(&phy, win_raw[i], ALPHY_PAY);
		win_tries[i]++;
		win_t[i] = now;
	}
	while (app_count && win_inflight() < AL_WIN && alphy_tx_space(&phy) > 0) {
		uint8_t t = app_type[app_tail];
		if (!win_send(t, app_pay[app_tail], app_n[app_tail], 1)) {
			break;
		}
		app_tail = (app_tail + 1u) % AL_APPQ;
		app_count--;
	}
	/* Keep ~2 ms of analog in the PHY so the DAC never plays silence. */
	if (alphy_tx_space(&phy) >= (ALPHY_Q - 2u)) {
		uint8_t idle[ALPHY_PAY];
		mac_pack(0, 0, 0, 0, idle);
		(void)alphy_send(&phy, idle, ALPHY_PAY);
	}
}

/** Parse one received PHY payload as a MAC frame. */
static void mac_in(const uint8_t *raw)
{
	uint8_t type = raw[0];
	uint16_t seq = (uint16_t)raw[2] | ((uint16_t)raw[3] << 8);
	uint16_t ack = (uint16_t)raw[4] | ((uint16_t)raw[5] << 8);
	unsigned payn = raw[8];
	win_ack(ack);
	if (type == 0) {
		return;
	}
	if (payn > AL_MAX_PAY) {
		payn = AL_MAX_PAY;
	}
	rx_ok++;
	if (seq != last_rx_seq || type == AL_PONG || type == AL_HELLO) {
		last_rx_seq = seq;
		if (type != AL_PONG && type != AL_HELLO) {
			rx_next = (uint16_t)(seq + 1u);
		}
		alink_dispatch(type, raw + AL_HDR, payn);
	}
}

/** Drain the PHY RX ring into the MAC. */
static void alink_drain(void)
{
	uint8_t raw[ALPHY_PAY];
	while (alphy_recv(&phy, raw, ALPHY_PAY) == ALPHY_PAY) {
		mac_in(raw);
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
	if (!share || !live) {
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
				(void)app_push(AL_TERM, pay, o);
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
		(void)app_push(AL_TERM, pay, o);
	} else {
		uint8_t cur[2];
		cur[0] = (uint8_t)tty_cursor_col();
		cur[1] = (uint8_t)tty_cursor_row();
		(void)app_push(AL_CUR, cur, 2);
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
	(void)app_push(AL_FACK, ack, 4);
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
		(void)app_push(AL_FNAK, nak, 4);
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
	if (!is_master) {
		char msg[64];
		ksnprintf(msg, sizeof(msg), "%s slave %s %s",
			AUDIOS_NAME, AUDIOS_VERSION_STRING, AUDIOS_SLAVE_BOARD);
		(void)app_push(AL_HELLO, (const uint8_t *)msg, (unsigned)strlen(msg));
	}
}

static void alink_dispatch(uint8_t type, const uint8_t *pay, unsigned n)
{
	switch (type) {
	case AL_HELLO:
		hello_in(pay, n);
		break;
	case AL_PING:
		(void)app_push(AL_PONG, pay, n);
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

static void send_hello(void)
{
	char msg[64];
	if (is_master) {
		ksnprintf(msg, sizeof(msg), "%s master %s %s",
			AUDIOS_NAME, AUDIOS_VERSION_STRING, AUDIOS_BOARD);
	} else {
		ksnprintf(msg, sizeof(msg), "%s slave %s %s",
			AUDIOS_NAME, AUDIOS_VERSION_STRING, AUDIOS_SLAVE_BOARD);
	}
	(void)app_push(AL_HELLO, (const uint8_t *)msg, (unsigned)strlen(msg));
	next_hello = pit_ticks() + 20u;
}

static int alink_start(int loop)
{
	unsigned i;
	loopback = loop ? 1 : 0;
	live = 1;
	alphy_reset(&phy);
	app_head = app_tail = app_count = 0;
	tx_seq = 1;
	tx_unacked = 1;
	rx_next = 0;
	last_rx_seq = 0xFFFFu;
	connected = 0;
	peer_name[0] = '\0';
	last_err[0] = '\0';
	pend_pong = 0;
	for (i = 0; i < AL_WIN; i++) {
		win_busy[i] = 0;
	}
	if (!loopback && hda_has_capture()) {
		hda_select_input(0);
		hda_cap_hook(alink_rx_pcm);
	} else {
		hda_cap_hook(0);
	}
	send_hello();
	alink_pump();
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
	hda_cap_hook(0);
	audio_dma_hold(0);
}

void alink_init(void)
{
	live = 0;
	loopback = 0;
	is_master = !AUDIOS_LINK_SLAVE;
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
	return app_push(AL_KEY, p, 2);
}

void alink_service(void)
{
	if (!live) {
		return;
	}
	alink_drain();
	if (is_master && !connected && pit_ticks() >= next_hello) {
		send_hello();
	}
	share_tick();
	alink_pump();
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

void alink_fill(int16_t *dst, uint32_t frames)
{
	alink_pump();
	alphy_gen(&phy, dst, frames, loopback);
	if (loopback) {
		alink_drain();
	}
}

void alink_rx_pcm(const int16_t *stereo, uint32_t frames)
{
	if (!live || loopback || stereo == NULL) {
		return;
	}
	alphy_adc(&phy, stereo, frames);
	alink_drain();
}

/** PHY + MAC roundtrip with no DAC (CI / `link test`). */
static int phy_selftest(void)
{
	uint8_t raw[ALPHY_PAY];
	uint8_t pay[8] = { 'a', 'u', 'd', 'i', 'O', 'S', '!' };
	int16_t pcm[ALPHY_FRAME_SAMP * 8u * 2u];
	alphy_reset(&phy);
	pend_pong = 0;
	rx_ok = 0;
	live = 1;
	loopback = 1;
	mac_pack(AL_PING, 1, pay, 7, raw);
	if (!alphy_send(&phy, raw, ALPHY_PAY)) {
		live = 0;
		return 0;
	}
	alphy_gen(&phy, pcm, ALPHY_FRAME_SAMP * 8u, 1);
	alink_drain();
	live = 0;
	loopback = 0;
	return pend_pong || rx_ok > 0;
}

static void cmd_status(void)
{
	tty_puts("Audio Link  Pulse PHY  48 kHz stereo 12-bit PAM\n");
	tty_printf("  ~976 kbit/s  1 ms frames  window %u  role %s\n",
		AL_WIN, is_master ? "MASTER (FX)" : "SLAVE (A7V333)");
	tty_printf("  %s  %s  %s  lock=%s\n",
		live ? "up" : "down",
		loopback ? "loopback" : "line",
		connected ? "peer" : "no peer",
		alphy_locked(&phy) ? "yes" : "no");
	if (peer_name[0]) {
		tty_printf("  peer: %s\n", peer_name);
	}
	tty_printf("  tx %u  rx %u  phy ok %u  bad %u\n",
		tx_ok, rx_ok, alphy_rx_ok(&phy), alphy_rx_bad(&phy));
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
	tty_puts("  cable: line-out -> peer line-in, both ways. Unplug speakers.\n");
}

static int wait_pong(uint32_t ms)
{
	uint64_t t0 = pit_ticks();
	int16_t tmp[ALPHY_FRAME_SAMP * 2u];
	unsigned i;
	if (loopback) {
		for (i = 0; i < 40u && !pend_pong; i++) {
			/* `tmp` holds interleaved stereo: 48 frames × 2 samples. */
			alink_fill(tmp, ALPHY_FRAME_SAMP);
		}
		return pend_pong;
	}
	while (pit_ticks() - t0 < ms) {
		audio_service();
		if (pend_pong) {
			return 1;
		}
	}
	return 0;
}

static int cmd_ping(void)
{
	uint8_t token = 0x5A;
	if (!live && !alink_start(loopback)) {
		return 0;
	}
	pend_pong = 0;
	(void)app_push(AL_PING, &token, 1);
	alink_pump();
	if (wait_pong(500)) {
		tty_puts("pong\n");
		return 1;
	}
	tty_set_color(TTY_COL_ERR);
	tty_puts("ping timeout\n");
	tty_set_color(TTY_COL_FG);
	return 0;
}

static int wait_idle(uint32_t ms)
{
	uint64_t t0 = pit_ticks();
	while ((app_count || win_inflight()) && pit_ticks() - t0 < ms) {
		audio_service();
		alink_service();
	}
	return app_count == 0 && win_inflight() == 0;
}

static int cmd_send(const char *path)
{
	uint8_t begin[4 + 48];
	const char *base;
	unsigned i, nl;
	uint32_t off = 0;
	uint8_t chunk[4 + 100];
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
	(void)app_push(AL_FBEGIN, begin, 4 + nl);
	for (;;) {
		uint32_t got = 0;
		while (app_count >= (AL_APPQ - 4u) || win_inflight() >= (AL_WIN - 2u)) {
			audio_service();
			alink_service();
		}
		if (!fs_read_at(path, off, chunk + 4, 100, &got) || got == 0) {
			break;
		}
		chunk[0] = (uint8_t)off;
		chunk[1] = (uint8_t)(off >> 8);
		chunk[2] = (uint8_t)(off >> 16);
		chunk[3] = (uint8_t)(off >> 24);
		(void)app_push(AL_FDATA, chunk, 4 + (unsigned)got);
		off += got;
		tty_printf("\r  %u bytes", off);
	}
	chunk[0] = (uint8_t)off;
	chunk[1] = (uint8_t)(off >> 8);
	chunk[2] = (uint8_t)(off >> 16);
	chunk[3] = (uint8_t)(off >> 24);
	(void)app_push(AL_FEND, chunk, 4);
	wait_idle(4000);
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
	if (strcmp(sub, "master") == 0) {
		is_master = 1;
		tty_puts("role MASTER (FX). link on to start.\n");
		return;
	}
	if (strcmp(sub, "slave") == 0) {
		is_master = 0;
		tty_puts("role SLAVE (A7V333). link on to listen.\n");
		return;
	}
	if (strcmp(sub, "on") == 0 || strcmp(sub, "start") == 0) {
		if (alink_start(0)) {
			tty_printf("link up %s — line-out / line-in. Unplug speakers.\n",
				is_master ? "MASTER" : "SLAVE");
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
		(void)app_push(AL_CMD, (const uint8_t *)line, o);
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
	tty_puts("link [status|master|slave|on|off|loop|ping|share|view|cmd|send|test]\n");
	tty_set_color(TTY_COL_FG);
}

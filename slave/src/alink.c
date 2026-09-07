#include "alink.h"
#include "alink_phy.h"
#include "audio.h"
#include "kbd.h"
#include "klib.h"
#include "pit.h"
#include "tty.h"
#include "version.h"

#include <stdint.h>

#define AL_HDR		9u
#define AL_MAX_PAY	(ALPHY_PAY - AL_HDR)
#define AL_APPQ		32u
#define AL_CELLS	2000u

#define AL_HELLO	0x01u
#define AL_PING		0x03u
#define AL_PONG		0x04u
#define AL_TERM		0x10u
#define AL_CUR		0x12u
#define AL_KEY		0x20u
#define AL_CMD		0x21u

#define AL_SHARE_MS	16u

static struct alphy phy;
static int live;
static int loopback;
static int share = 1;
static int view;
static int connected;
static uint32_t tx_ok, rx_ok;
static uint16_t tx_seq = 1;
static uint16_t rx_next;
static uint16_t last_rx_seq = 0xFFFFu;

static uint8_t app_type[AL_APPQ];
static uint8_t app_pay[AL_APPQ][AL_MAX_PAY];
static unsigned app_n[AL_APPQ];
static unsigned app_head, app_tail, app_count;

static uint8_t last_ch[AL_CELLS];
static uint8_t last_pal[AL_CELLS];
static int snap_ok;
static uint32_t next_share;

static int pend_key, pend_key_n, pend_pong, pend_cmd_ready;
static char pend_cmd[120];
static char peer_name[48];
static char last_err[80];

static void (*run_line)(const char *line);

void alink_set_runner(void (*fn)(const char *line));

/** Pack a slave MAC header. Flag 0x02 = slave. */
static void mac_pack(uint8_t type, uint16_t seq, const uint8_t *pay, unsigned payn, uint8_t *out)
{
	unsigned i;
	if (payn > AL_MAX_PAY) {
		payn = AL_MAX_PAY;
	}
	memset(out, 0, ALPHY_PAY);
	out[0] = type;
	out[1] = 0x02;
	out[2] = (uint8_t)seq;
	out[3] = (uint8_t)(seq >> 8);
	out[4] = (uint8_t)rx_next;
	out[5] = (uint8_t)(rx_next >> 8);
	out[8] = (uint8_t)payn;
	for (i = 0; i < payn; i++) {
		out[AL_HDR + i] = pay[i];
	}
}

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

static void pump(void)
{
	if (!live) {
		return;
	}
	while (app_count && alphy_tx_space(&phy) > 0) {
		uint8_t raw[ALPHY_PAY];
		mac_pack(app_type[app_tail], tx_seq, app_pay[app_tail], app_n[app_tail], raw);
		if (!alphy_send(&phy, raw, ALPHY_PAY)) {
			break;
		}
		tx_seq++;
		tx_ok++;
		app_tail = (app_tail + 1u) % AL_APPQ;
		app_count--;
	}
	if (alphy_tx_space(&phy) >= (ALPHY_Q - 2u)) {
		uint8_t idle[ALPHY_PAY];
		mac_pack(0, 0, 0, 0, idle);
		(void)alphy_send(&phy, idle, ALPHY_PAY);
	}
}

static void send_hello(void)
{
	char msg[64];
	ksnprintf(msg, sizeof(msg), "%s slave %s %s",
		AUDIOS_NAME, AUDIOS_VERSION_STRING, AUDIOS_SLAVE_BOARD);
	(void)app_push(AL_HELLO, (const uint8_t *)msg, (unsigned)strlen(msg));
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
	send_hello();
}

static void apply_term(const uint8_t *pay, unsigned n)
{
	unsigned i;
	if (!view) {
		return;
	}
	for (i = 0; i + 4u <= n; i += 4u) {
		tty_put_xy(pay[i + 1u], pay[i], (char)pay[i + 2u], TTY_COL_FG);
	}
}

static void dispatch(uint8_t type, const uint8_t *pay, unsigned n)
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
		if (view && n >= 2) {
			tty_set_cursor(pay[0], pay[1]);
		}
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
	default:
		break;
	}
}

static void drain(void)
{
	uint8_t raw[ALPHY_PAY];
	while (alphy_recv(&phy, raw, ALPHY_PAY) == ALPHY_PAY) {
		uint8_t type = raw[0];
		uint16_t seq = (uint16_t)raw[2] | ((uint16_t)raw[3] << 8);
		unsigned payn = raw[8];
		if (type == 0) {
			continue;
		}
		if (payn > AL_MAX_PAY) {
			payn = AL_MAX_PAY;
		}
		rx_ok++;
		if (seq != last_rx_seq || type == AL_HELLO || type == AL_PONG) {
			last_rx_seq = seq;
			rx_next = (uint16_t)(seq + 1u);
			dispatch(type, raw + AL_HDR, payn);
		}
	}
}

static void share_tick(void)
{
	unsigned cols = tty_cols();
	unsigned rows = tty_rows();
	unsigned r, c, o = 0;
	uint8_t pay[AL_MAX_PAY];
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
			unsigned idx = r * cols + c;
			if (idx >= AL_CELLS) {
				break;
			}
			tty_cell_at(c, r, &ch, &rgb);
			if (last_ch[idx] == (uint8_t)ch) {
				continue;
			}
			if (o + 4u > AL_MAX_PAY) {
				(void)app_push(AL_TERM, pay, o);
				return;
			}
			pay[o++] = (uint8_t)r;
			pay[o++] = (uint8_t)c;
			pay[o++] = (uint8_t)ch;
			pay[o++] = 0;
			last_ch[idx] = (uint8_t)ch;
		}
	}
	if (o) {
		(void)app_push(AL_TERM, pay, o);
	}
}

static int alink_start(int loop)
{
	audio_stop();
	loopback = loop ? 1 : 0;
	live = 1;
	alphy_reset(&phy);
	app_head = app_tail = app_count = 0;
	tx_seq = 1;
	connected = 0;
	peer_name[0] = '\0';
	last_err[0] = '\0';
	send_hello();
	pump();
	if (!loopback && audio_present()) {
		if (!audio_start_link(alink_fill, alink_rx_pcm)) {
			live = 0;
			ksnprintf(last_err, sizeof(last_err), "%s", "codec DMA would not start");
			return 0;
		}
	}
	return 1;
}

int alink_start_slave(void)
{
	return alink_start(audio_present() ? 0 : 1);
}

static void alink_stop(void)
{
	live = 0;
	audio_stop();
}

void alink_init(void)
{
	live = 0;
	share = 1;
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

void alink_fill(int16_t *dst, uint32_t frames)
{
	pump();
	alphy_gen(&phy, dst, frames, loopback);
	if (loopback) {
		drain();
	}
}

void alink_rx_pcm(const int16_t *stereo, uint32_t frames)
{
	if (!live || loopback || stereo == NULL) {
		return;
	}
	alphy_adc(&phy, stereo, frames);
	drain();
}

void alink_service(void)
{
	if (!live) {
		return;
	}
	drain();
	share_tick();
	pump();
	audio_service();
}

void alink_poll(void)
{
	if (pend_key_n) {
		kbd_inject(pend_key);
		pend_key_n = 0;
	}
	if (pend_cmd_ready && run_line) {
		tty_set_color(TTY_COL_ACCENT);
		tty_printf("[link] %s\n", pend_cmd);
		tty_set_color(TTY_COL_FG);
		run_line(pend_cmd);
		pend_cmd_ready = 0;
	}
}

void alink_set_runner(void (*fn)(const char *line))
{
	run_line = fn;
}

static int phy_selftest(void)
{
	uint8_t raw[ALPHY_PAY];
	uint8_t pay[8] = { 'a', 'u', 'd', 'i', 'O', 'S', '!' };
	int16_t pcm[ALPHY_FRAME_SAMP * 8u * 2u];
	audio_stop();
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
	drain();
	live = 0;
	loopback = 0;
	return pend_pong || rx_ok > 0;
}

static int cmd_ping(void)
{
	uint8_t token = 0x5A;
	int16_t tmp[ALPHY_FRAME_SAMP * 2u];
	unsigned i;
	if (!live && !alink_start(1)) {
		return 0;
	}
	pend_pong = 0;
	(void)app_push(AL_PING, &token, 1);
	pump();
	/* 40 superframes of loopback PCM (~40 ms). Buffer is interleaved stereo. */
	for (i = 0; i < 40u && !pend_pong; i++) {
		alink_fill(tmp, ALPHY_FRAME_SAMP);
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

void alink_cmd(int argc, char **argv)
{
	const char *sub = (argc > 1) ? argv[1] : "";
	if (sub[0] == '\0' || strcmp(sub, "status") == 0) {
		tty_puts("Audio Link  Pulse PHY  48 kHz stereo 12-bit PAM\n");
		tty_puts("  ~976 kbit/s  role SLAVE (A7V333)\n");
		tty_printf("  %s  %s  %s  lock=%s\n",
			live ? "up" : "down",
			loopback ? "loopback" : "line",
			connected ? "peer" : "no peer",
			alphy_locked(&phy) ? "yes" : "no");
		if (peer_name[0]) {
			tty_printf("  peer: %s\n", peer_name);
		}
		tty_printf("  tx %u  rx %u  codec %s\n",
			tx_ok, rx_ok, audio_name());
		tty_puts("  cable: line-out -> FX line-in, both ways.\n");
		return;
	}
	if (strcmp(sub, "on") == 0 || strcmp(sub, "slave") == 0) {
		if (alink_start(0)) {
			tty_puts("link up SLAVE (line-out / line-in)\n");
		} else {
			tty_printf("link: %s\n", last_err);
		}
		return;
	}
	if (strcmp(sub, "off") == 0) {
		alink_stop();
		tty_puts("link down\n");
		return;
	}
	if (strcmp(sub, "loop") == 0) {
		if (alink_start(1)) {
			tty_puts("link loopback\n");
		}
		return;
	}
	if (strcmp(sub, "ping") == 0) {
		cmd_ping();
		return;
	}
	if (strcmp(sub, "share") == 0) {
		share = 1;
		snap_ok = 0;
		tty_puts("sharing this terminal\n");
		return;
	}
	if (strcmp(sub, "view") == 0) {
		view = 1;
		tty_puts("link view — Ctrl-X local\n");
		return;
	}
	if (strcmp(sub, "test") == 0) {
		if (!phy_selftest()) {
			tty_puts("link test: PHY roundtrip failed\n");
			return;
		}
		if (!alink_start(1) || !cmd_ping()) {
			alink_stop();
			return;
		}
		alink_stop();
		tty_set_color(TTY_COL_AUDIO);
		tty_puts("link test ok\n");
		tty_set_color(TTY_COL_FG);
		return;
	}
	tty_puts("link [status|on|off|loop|ping|share|view|test]\n");
}

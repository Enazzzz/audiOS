#include "tetris.h"
#include "audio.h"
#include "fs.h"
#include "kbd.h"
#include "klib.h"
#include "pit.h"
#include "serial.h"
#include "tty.h"

#include <stdint.h>

#define WELL_W		10
#define WELL_H		20
#define N_KIND		7
#define DAS_DELAY	16
#define DAS_PERIOD	6
#define CLEAR_FRAMES	20
#define FRAME_MS	16u
#define SCORE_MAX	8
#define SCORE_PATH	"D:/tetris.scr"

/* Nintendo Rotation System colours (VGA). */
#define COL_BG		0x101014u
#define COL_HUD		0xD6DCE0u
#define COL_DIM		0x8A9098u
#define COL_BORDER	0x606870u
#define COL_EMPTY	0x18181Cu
#define COL_FLASH	0xF0F0E8u

typedef enum {
	PK_T = 0,
	PK_J,
	PK_Z,
	PK_O,
	PK_S,
	PK_L,
	PK_I
} piece_kind_t;

typedef enum {
	ST_PLAY = 0,
	ST_ARE,
	ST_CLEAR,
	ST_PAUSE,
	ST_OVER,
	ST_NAME
} game_state_t;

/*
 * NRS right-handed (NES). Four blocks, origin at the top-left of a 4×4.
 * No wall kicks. I is the long bar on row 1; O sits in the centre 2×2.
 */
static const int8_t nrs[N_KIND][4][4][2] = {
	[PK_T] = {
		{{1, 0}, {0, 1}, {1, 1}, {2, 1}},
		{{1, 0}, {1, 1}, {2, 1}, {1, 2}},
		{{0, 1}, {1, 1}, {2, 1}, {1, 2}},
		{{1, 0}, {0, 1}, {1, 1}, {1, 2}},
	},
	[PK_J] = {
		{{0, 0}, {1, 0}, {2, 0}, {2, 1}},
		{{1, 0}, {1, 1}, {0, 2}, {1, 2}},
		{{0, 1}, {0, 2}, {1, 2}, {2, 2}},
		{{1, 0}, {2, 0}, {1, 1}, {1, 2}},
	},
	[PK_Z] = {
		{{0, 0}, {1, 0}, {1, 1}, {2, 1}},
		{{2, 0}, {1, 1}, {2, 1}, {1, 2}},
		{{0, 0}, {1, 0}, {1, 1}, {2, 1}},
		{{2, 0}, {1, 1}, {2, 1}, {1, 2}},
	},
	[PK_O] = {
		{{1, 0}, {2, 0}, {1, 1}, {2, 1}},
		{{1, 0}, {2, 0}, {1, 1}, {2, 1}},
		{{1, 0}, {2, 0}, {1, 1}, {2, 1}},
		{{1, 0}, {2, 0}, {1, 1}, {2, 1}},
	},
	[PK_S] = {
		{{1, 0}, {2, 0}, {0, 1}, {1, 1}},
		{{1, 0}, {1, 1}, {2, 1}, {2, 2}},
		{{1, 0}, {2, 0}, {0, 1}, {1, 1}},
		{{1, 0}, {1, 1}, {2, 1}, {2, 2}},
	},
	[PK_L] = {
		{{0, 0}, {1, 0}, {2, 0}, {0, 1}},
		{{0, 0}, {1, 0}, {1, 1}, {1, 2}},
		{{2, 1}, {0, 2}, {1, 2}, {2, 2}},
		{{1, 0}, {1, 1}, {1, 2}, {2, 2}},
	},
	[PK_I] = {
		{{0, 1}, {1, 1}, {2, 1}, {3, 1}},
		{{2, 0}, {2, 1}, {2, 2}, {2, 3}},
		{{0, 1}, {1, 1}, {2, 1}, {3, 1}},
		{{2, 0}, {2, 1}, {2, 2}, {2, 3}},
	},
};

static const uint32_t kind_rgb[N_KIND] = {
	[PK_T] = 0xA050C0u,
	[PK_J] = 0x4050C0u,
	[PK_Z] = 0xC04040u,
	[PK_O] = 0xC0C040u,
	[PK_S] = 0x40C050u,
	[PK_L] = 0xC08040u,
	[PK_I] = 0x40C0C0u,
};

static uint8_t well[WELL_H][WELL_W];
static int px;
static int py;
static int prot;
static piece_kind_t cur;
static piece_kind_t nxt;
static piece_kind_t last_kind;
static unsigned start_level;
static unsigned level;
static unsigned lines;
static unsigned next_level_at;
static uint32_t score;
static unsigned grav_left;
static int das;
static int das_dir;
static unsigned wait_left;
static game_state_t state;
static game_state_t paused_from;
static uint8_t clear_row[WELL_H];
static int running;
static uint32_t rng_state;
static int esc;
static unsigned esc_num;
static uint64_t ser_left_until;
static uint64_t ser_right_until;
static uint64_t ser_down_until;
static int well_x;
static int well_y;
static int dirty;

struct hiscore {
	char name[4];
	uint32_t score;
	unsigned lines;
	unsigned level;
};

static struct hiscore scores[SCORE_MAX];
static unsigned nscore;
static char name_in[4];
static unsigned name_n;

/** NTSC gravity table (frames per row) from NES Tetris.
 * Levels 19–28 stay at 2G; 29 is 1G. That plateau is NES, not a bug.
 */
static unsigned grav_frames(unsigned lv)
{
	static const uint8_t low[10] = {48, 43, 38, 33, 28, 23, 18, 13, 8, 6};
	if (lv >= 29u) {
		return 1;
	}
	if (lv >= 19u) {
		return 2;
	}
	if (lv >= 16u) {
		return 3;
	}
	if (lv >= 13u) {
		return 4;
	}
	if (lv >= 10u) {
		return 5;
	}
	return low[lv];
}

/** Load D:/tetris.scr (name score lines level per line). */
static void scores_load(void)
{
	nscore = 0;
	char raw[512];
	uint32_t n = 0;
	if (!fs_read_file(SCORE_PATH, raw, sizeof(raw) - 1u, &n) || n == 0) {
		return;
	}
	raw[n] = '\0';
	char *p = raw;
	while (*p && nscore < SCORE_MAX) {
		while (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t') {
			p++;
		}
		if (*p == '\0') {
			break;
		}
		char nm[4];
		unsigned k = 0;
		while (*p && *p != ' ' && k < 3) {
			nm[k++] = *p++;
		}
		nm[k] = '\0';
		while (*p == ' ') {
			p++;
		}
		uint32_t sc = 0;
		unsigned ln = 0;
		unsigned lv = 0;
		while (*p >= '0' && *p <= '9') {
			sc = sc * 10u + (unsigned)(*p - '0');
			p++;
		}
		while (*p == ' ') {
			p++;
		}
		while (*p >= '0' && *p <= '9') {
			ln = ln * 10u + (unsigned)(*p - '0');
			p++;
		}
		while (*p == ' ') {
			p++;
		}
		while (*p >= '0' && *p <= '9') {
			lv = lv * 10u + (unsigned)(*p - '0');
			p++;
		}
		ksnprintf(scores[nscore].name, sizeof(scores[nscore].name), "%s", nm[0] ? nm : "---");
		scores[nscore].score = sc;
		scores[nscore].lines = ln;
		scores[nscore].level = lv;
		nscore++;
		while (*p && *p != '\n') {
			p++;
		}
	}
}

/** Write the table back to the data volume. */
static void scores_save(void)
{
	char raw[512];
	unsigned o = 0;
	raw[0] = '\0';
	for (unsigned i = 0; i < nscore; i++) {
		char line[80];
		ksnprintf(line, sizeof(line), "%s %u %u %u\n",
			scores[i].name, scores[i].score, scores[i].lines, scores[i].level);
		size_t L = strlen(line);
		if (o + L + 1 >= sizeof(raw)) {
			break;
		}
		memcpy(raw + o, line, L);
		o += (unsigned)L;
	}
	(void)fs_write_file(SCORE_PATH, raw, o);
}

/** True if this run belongs on the board. */
static int score_qualifies(void)
{
	if (score == 0) {
		return 0;
	}
	if (nscore < SCORE_MAX) {
		return 1;
	}
	return score > scores[nscore - 1].score;
}

/** Insert the current run under `name`. */
static void score_insert(const char *name)
{
	unsigned i = nscore;
	if (i > SCORE_MAX) {
		i = SCORE_MAX;
	}
	while (i > 0 && score > scores[i - 1].score) {
		if (i < SCORE_MAX) {
			scores[i] = scores[i - 1];
		}
		i--;
	}
	if (i >= SCORE_MAX) {
		return;
	}
	ksnprintf(scores[i].name, sizeof(scores[i].name), "%s", name);
	scores[i].score = score;
	scores[i].lines = lines;
	scores[i].level = level;
	if (nscore < SCORE_MAX) {
		nscore++;
	}
	scores_save();
}

/** Print the table (shell `tetris scores`). */
static void scores_print(void)
{
	scores_load();
	tty_puts("NES tetris scores (D:/tetris.scr)\n");
	if (nscore == 0) {
		tty_puts("  (empty)\n");
	} else {
		for (unsigned i = 0; i < nscore; i++) {
			tty_printf("  %u. %s  %u  L%u  lv%u\n",
				i + 1, scores[i].name, scores[i].score,
				scores[i].lines, scores[i].level);
		}
	}
	tty_set_color(TTY_COL_DIM);
	tty_puts("Lv 19-28 stay at 2G; lv 29 is 1G. That is NES, not a bug.\n");
	tty_set_color(TTY_COL_FG);
}

/** NTSC-ish first level-up line count (NES start-level rule). */
static unsigned first_goal(unsigned start)
{
	if (start < 10u) {
		return start * 10u + 10u;
	}
	unsigned cut = (start * 10u > 50u) ? (start * 10u - 50u) : 100u;
	return cut > 100u ? cut : 100u;
}

/** ARE: 10 frames plus 2 per 4 rows above the bottom, capped at 18. */
static unsigned are_frames(int lock_y)
{
	int from_bottom = (WELL_H - 1) - lock_y;
	if (from_bottom < 0) {
		from_bottom = 0;
	}
	unsigned extra = ((unsigned)from_bottom / 4u) * 2u;
	unsigned d = 10u + extra;
	if (d > 18u) {
		d = 18u;
	}
	return d;
}

/** Integer xorshift; seeded from the PIT so the first piece is not fixed. */
static uint32_t rng(void)
{
	rng_state ^= rng_state << 13;
	rng_state ^= rng_state >> 17;
	rng_state ^= rng_state << 5;
	return rng_state;
}

/**
 * NES piece RNG: roll 0–7, reroll if 7 or equal to the last piece,
 * otherwise a second 0–6 roll.
 */
static piece_kind_t roll_piece(void)
{
	unsigned r = rng() & 7u;
	if (r == 7u || (piece_kind_t)r == last_kind) {
		r = rng() % 7u;
	}
	last_kind = (piece_kind_t)r;
	return last_kind;
}

/** True if this block is outside the well or on a filled cell. y < 0 is open (hidden rows). */
static int blocked_at(int x, int y)
{
	if (x < 0 || x >= WELL_W || y >= WELL_H) {
		return 1;
	}
	if (y < 0) {
		return 0;
	}
	return well[y][x] != 0;
}

/** Collision test for a kind/rotation at a well origin. */
static int collides(piece_kind_t k, int rot, int x, int y)
{
	int r = rot & 3;
	for (int i = 0; i < 4; i++) {
		int bx = x + (int)nrs[k][r][i][0];
		int by = y + (int)nrs[k][r][i][1];
		if (blocked_at(bx, by)) {
			return 1;
		}
	}
	return 0;
}

/** Stamp the active piece into the well (values 1–7). */
static void lock_piece(void)
{
	int r = prot & 3;
	for (int i = 0; i < 4; i++) {
		int bx = px + (int)nrs[cur][r][i][0];
		int by = py + (int)nrs[cur][r][i][1];
		if (by >= 0 && by < WELL_H && bx >= 0 && bx < WELL_W) {
			well[by][bx] = (uint8_t)(cur + 1);
		}
	}
}

/** Try a translation. Returns 1 if the piece moved. */
static int try_move(int dx, int dy)
{
	if (collides(cur, prot, px + dx, py + dy)) {
		return 0;
	}
	px += dx;
	py += dy;
	dirty = 1;
	return 1;
}

/** NRS rotate with no kicks: ignore the turn if it collides. */
static void try_rot(int dir)
{
	int nr = (prot + dir) & 3;
	if (!collides(cur, nr, px, py)) {
		prot = nr;
		dirty = 1;
	}
}

/** Place a new piece at NES spawn (highest block on the top row). */
static void spawn_piece(void)
{
	cur = nxt;
	nxt = roll_piece();
	prot = 0;
	px = 3;
	py = 0;
	grav_left = grav_frames(level);
	if (collides(cur, prot, px, py)) {
		name_n = 0;
		ksnprintf(name_in, sizeof(name_in), "AAA");
		state = score_qualifies() ? ST_NAME : ST_OVER;
	} else {
		state = ST_PLAY;
	}
	dirty = 1;
}

/** Mark full rows; return how many. */
static int mark_clears(void)
{
	int n = 0;
	for (int y = 0; y < WELL_H; y++) {
		int full = 1;
		for (int x = 0; x < WELL_W; x++) {
			if (well[y][x] == 0) {
				full = 0;
				break;
			}
		}
		clear_row[y] = (uint8_t)full;
		if (full) {
			n++;
		}
	}
	return n;
}

/** Collapse marked rows from the bottom. */
static void apply_clears(void)
{
	int dst = WELL_H - 1;
	for (int y = WELL_H - 1; y >= 0; y--) {
		if (clear_row[y]) {
			continue;
		}
		if (dst != y) {
			for (int x = 0; x < WELL_W; x++) {
				well[dst][x] = well[y][x];
			}
		}
		dst--;
	}
	while (dst >= 0) {
		for (int x = 0; x < WELL_W; x++) {
			well[dst][x] = 0;
		}
		dst--;
	}
	for (int y = 0; y < WELL_H; y++) {
		clear_row[y] = 0;
	}
}

/** NES line score: 40 / 100 / 300 / 1200 × (level + 1). */
static void add_line_score(int n)
{
	static const unsigned base[5] = {0, 40, 100, 300, 1200};
	if (n < 1) {
		return;
	}
	if (n > 4) {
		n = 4;
	}
	score += (uint32_t)base[n] * (level + 1u);
}

/** After a batch of lines, maybe bump the level. */
static void add_lines(int n)
{
	lines += (unsigned)n;
	while (lines >= next_level_at) {
		level++;
		next_level_at += 10u;
	}
}

/** Lock, then line-clear delay or ARE. */
static void piece_landed(void)
{
	lock_piece();
	int n = mark_clears();
	if (n > 0) {
		add_line_score(n);
		add_lines(n);
		state = ST_CLEAR;
		wait_left = CLEAR_FRAMES;
	} else {
		state = ST_ARE;
		wait_left = are_frames(py);
	}
	dirty = 1;
}

/** One gravity tick: move down or lock (no lock delay). */
static void gravity_tick(int soft)
{
	if (try_move(0, 1)) {
		if (soft) {
			score += 1;
		}
		return;
	}
	piece_landed();
}

/** DAS left/right: 16-frame delay, then every 6; blocked tap charges to 16. */
static void das_tick(int left, int right)
{
	int dir = 0;
	if (left && !right) {
		dir = -1;
	} else if (right && !left) {
		dir = 1;
	}
	if (dir == 0) {
		das = 0;
		das_dir = 0;
		return;
	}
	if (das_dir != dir) {
		das_dir = dir;
		das = 0;
		if (!try_move(dir, 0)) {
			das = DAS_DELAY;
		}
		return;
	}
	das++;
	if (das >= DAS_DELAY && ((das - DAS_DELAY) % DAS_PERIOD) == 0) {
		if (!try_move(dir, 0)) {
			das = DAS_DELAY;
		}
	}
}

/** Serial CSI hold expires; PS/2 uses kbd_held. */
static int arrow_held(int key)
{
	uint64_t now = pit_ticks();
	if (kbd_held(key)) {
		return 1;
	}
	if (key == KBD_LEFT && now < ser_left_until) {
		return 1;
	}
	if (key == KBD_RIGHT && now < ser_right_until) {
		return 1;
	}
	if (key == KBD_DOWN && now < ser_down_until) {
		return 1;
	}
	return 0;
}

/** Remember a serial arrow as held for a couple of frames (no key-up on COM1). */
static void ser_hold(int key)
{
	uint64_t until = pit_ticks() + 40u;
	if (key == KBD_LEFT) {
		ser_left_until = until;
	} else if (key == KBD_RIGHT) {
		ser_right_until = until;
	} else if (key == KBD_DOWN) {
		ser_down_until = until;
	}
}

/** Draw a NUL-terminated string with tty_put_xy. */
static void put_str(unsigned col, unsigned row, const char *s, uint32_t rgb)
{
	while (*s != '\0') {
		tty_put_xy(col++, row, *s++, rgb);
	}
}

/** Write `s` then pad with spaces so a shorter string cannot leave leftovers. */
static void put_str_pad(unsigned col, unsigned row, const char *s, unsigned width, uint32_t rgb)
{
	unsigned i = 0;
	while (s[i] != '\0' && i < width) {
		tty_put_xy(col + i, row, s[i], rgb);
		i++;
	}
	while (i < width) {
		tty_put_xy(col + i, row, ' ', COL_BG);
		i++;
	}
}

/** Unsigned decimal into a fixed-width field (pads so digits do not ghost). */
static void put_field(unsigned col, unsigned row, uint32_t n, unsigned width, uint32_t rgb)
{
	char tmp[16];
	ksnprintf(tmp, sizeof(tmp), "%u", n);
	put_str_pad(col, row, tmp, width, rgb);
}

/** Colour for a well cell id (0 empty, 1–7 locked kinds). */
static uint32_t cell_rgb(uint8_t id)
{
	if (id == 0) {
		return COL_EMPTY;
	}
	unsigned k = (unsigned)(id - 1);
	if (k >= N_KIND) {
		return COL_DIM;
	}
	return kind_rgb[k];
}

/** Paint HUD, well, active piece, and next preview. */
static void paint(void)
{
	unsigned cols = tty_cols();
	unsigned rows = tty_rows();
	put_str(0, 0, "NES tetris", TTY_COL_ACCENT);
	put_str(12, 0, "  X/Up CW  Z CCW  arrows  Down soft  P pause  Q quit", COL_DIM);
	put_str(0, 1, "LEVEL ", COL_HUD);
	put_field(6, 1, level, 4, TTY_COL_ACCENT);
	put_str(11, 1, "SCORE ", COL_HUD);
	put_field(17, 1, score, 8, TTY_COL_ACCENT);
	put_str(26, 1, "LINES ", COL_HUD);
	put_field(32, 1, lines, 6, TTY_COL_ACCENT);
	put_str_pad(0, 2, "19-28 stay 2G, 29 is 1G (NES)", 40, COL_DIM);

	if (well_x < 1) {
		well_x = 2;
	}
	if (well_y < 2) {
		well_y = 3;
	}

	for (int y = -1; y <= WELL_H; y++) {
		for (int x = -1; x <= WELL_W; x++) {
			unsigned col = (unsigned)(well_x + (x + 1) * 2);
			unsigned row = (unsigned)(well_y + y + 1);
			if (col + 1 >= cols || row >= rows) {
				continue;
			}
			if (x < 0 || x >= WELL_W || y < 0 || y >= WELL_H) {
				tty_put_xy(col, row, '[', COL_BORDER);
				tty_put_xy(col + 1u, row, ']', COL_BORDER);
				continue;
			}
			uint8_t id = well[y][x];
			uint32_t rgb = cell_rgb(id);
			char a = (id || (state == ST_CLEAR && clear_row[y])) ? '[' : ' ';
			char b = (id || (state == ST_CLEAR && clear_row[y])) ? ']' : ' ';
			if (state == ST_CLEAR && clear_row[y]) {
				rgb = COL_FLASH;
				a = '[';
				b = ']';
			}
			tty_put_xy(col, row, a, rgb);
			tty_put_xy(col + 1u, row, b, rgb);
		}
	}

	if (state == ST_PLAY || state == ST_PAUSE) {
		int r = prot & 3;
		for (int i = 0; i < 4; i++) {
			int bx = px + (int)nrs[cur][r][i][0];
			int by = py + (int)nrs[cur][r][i][1];
			if (bx < 0 || bx >= WELL_W || by < 0 || by >= WELL_H) {
				continue;
			}
			unsigned col = (unsigned)(well_x + (bx + 1) * 2);
			unsigned row = (unsigned)(well_y + by + 1);
			tty_put_xy(col, row, '[', kind_rgb[cur]);
			tty_put_xy(col + 1u, row, ']', kind_rgb[cur]);
		}
	}

	put_str((unsigned)(well_x + WELL_W * 2 + 6), (unsigned)well_y, "NEXT", COL_HUD);
	{
		int occ[4][4];
		memset(occ, 0, sizeof(occ));
		for (int i = 0; i < 4; i++) {
			int bx = (int)nrs[nxt][0][i][0];
			int by = (int)nrs[nxt][0][i][1];
			if (bx >= 0 && bx < 4 && by >= 0 && by < 4) {
				occ[by][bx] = 1;
			}
		}
		for (int y = 0; y < 4; y++) {
			for (int x = 0; x < 4; x++) {
				unsigned col = (unsigned)(well_x + WELL_W * 2 + 6 + x * 2);
				unsigned row = (unsigned)(well_y + 2 + y);
				if (occ[y][x]) {
					tty_put_xy(col, row, '[', kind_rgb[nxt]);
					tty_put_xy(col + 1u, row, ']', kind_rgb[nxt]);
				} else {
					tty_put_xy(col, row, ' ', COL_BG);
					tty_put_xy(col + 1u, row, ' ', COL_BG);
				}
			}
		}
	}

	if (state == ST_PAUSE) {
		put_str_pad((unsigned)well_x, (unsigned)(well_y + WELL_H / 2), "PAUSED", 20, TTY_COL_AUDIO);
	} else if (state == ST_OVER) {
		put_str_pad((unsigned)well_x, (unsigned)(well_y + WELL_H / 2), "GAME OVER  Q quit", 20, TTY_COL_ERR);
		put_str_pad((unsigned)well_x, (unsigned)(well_y + WELL_H / 2 + 1), "", 20, COL_BG);
	} else if (state == ST_NAME) {
		put_str_pad((unsigned)well_x, (unsigned)(well_y + WELL_H / 2), "HIGH SCORE  name:", 20, TTY_COL_AUDIO);
		put_str_pad((unsigned)well_x, (unsigned)(well_y + WELL_H / 2 + 1), name_in, 4, TTY_COL_ACCENT);
		put_str((unsigned)well_x + 4u, (unsigned)(well_y + WELL_H / 2 + 1), "  Enter save", COL_DIM);
	} else {
		put_str_pad((unsigned)well_x, (unsigned)(well_y + WELL_H / 2), "", 20, COL_BG);
		put_str_pad((unsigned)well_x, (unsigned)(well_y + WELL_H / 2 + 1), "", 20, COL_BG);
	}
	{
		unsigned sc = (unsigned)(well_x + WELL_W * 2 + 6);
		unsigned sr = (unsigned)well_y + 8u;
		put_str(sc, sr, "SCORES", COL_HUD);
		for (unsigned i = 0; i < 8; i++) {
			char line[40];
			if (i < nscore) {
				ksnprintf(line, sizeof(line), "%s %u", scores[i].name, scores[i].score);
			} else {
				line[0] = '\0';
			}
			put_str_pad(sc, sr + 1 + i, line, 18, COL_DIM);
		}
	}
	dirty = 0;
}

/** One ~60 Hz logic step while a piece is falling. */
static void play_frame(void)
{
	das_tick(arrow_held(KBD_LEFT), arrow_held(KBD_RIGHT));
	int soft = arrow_held(KBD_DOWN);
	unsigned delay = grav_frames(level);
	if (soft && delay > 2u) {
		delay = 2u;
	}
	if (grav_left == 0 || grav_left > delay) {
		grav_left = delay;
	}
	if (grav_left > 0) {
		grav_left--;
	}
	if (grav_left == 0) {
		gravity_tick(soft);
		if (state == ST_PLAY) {
			grav_left = delay;
		}
	}
}

/** Advance ARE / line-clear timers. */
static void wait_frame(void)
{
	if (wait_left > 0) {
		wait_left--;
		return;
	}
	if (state == ST_CLEAR) {
		apply_clears();
		state = ST_ARE;
		wait_left = are_frames(0);
		dirty = 1;
		return;
	}
	if (state == ST_ARE) {
		spawn_piece();
	}
}

/** Map a decoded key onto rotate / pause / quit. DAS uses hold state. */
static void feed_key(int c)
{
	if (c == 'q' || c == 'Q') {
		running = 0;
		return;
	}
	if (state == ST_NAME) {
		if (c == '\n' || c == '\r') {
			if (name_in[0] == '\0') {
				ksnprintf(name_in, sizeof(name_in), "AAA");
			}
			score_insert(name_in);
			state = ST_OVER;
			dirty = 1;
			return;
		}
		if ((c == '\b' || c == 0x7F) && name_n > 0) {
			name_n--;
			name_in[name_n] = '\0';
			dirty = 1;
			return;
		}
		if (c >= 'a' && c <= 'z') {
			c = c - 'a' + 'A';
		}
		if (c >= 'A' && c <= 'Z' && name_n < 3) {
			name_in[name_n++] = (char)c;
			name_in[name_n] = '\0';
			dirty = 1;
		}
		return;
	}
	if (c == 'p' || c == 'P') {
		if (state == ST_PAUSE) {
			state = paused_from;
			dirty = 1;
		} else if (state != ST_OVER) {
			paused_from = state;
			state = ST_PAUSE;
			dirty = 1;
		}
		return;
	}
	if (state == ST_PAUSE || state == ST_OVER || state == ST_NAME) {
		return;
	}
	if (state != ST_PLAY) {
		return;
	}
	if (c == 'x' || c == 'X' || c == KBD_UP) {
		try_rot(1);
		return;
	}
	if (c == 'z' || c == 'Z') {
		try_rot(-1);
		return;
	}
	if (c == KBD_LEFT) {
		return;
	}
	if (c == KBD_RIGHT) {
		return;
	}
	if (c == KBD_DOWN) {
		return;
	}
}

/**
 * Decode COM1 CSI/SS3 arrows the same way the shell does, then feed keys.
 * Arrow CSI also arms a short hold so DAS/soft-drop work without key-up.
 */
static void feed_serial(int c)
{
	if (c == 0x1B) {
		esc = 1;
		return;
	}
	if (esc == 1) {
		if (c == '[') {
			esc = 2;
			esc_num = 0;
			return;
		}
		if (c == 'O') {
			esc = 3;
			return;
		}
		esc = 0;
	} else if (esc == 2 || esc == 3) {
		if (esc == 2 && c >= '0' && c <= '9') {
			esc_num = esc_num * 10u + (unsigned)(c - '0');
			return;
		}
		esc = 0;
		if (c == 'A') {
			feed_key(KBD_UP);
			return;
		}
		if (c == 'B') {
			ser_hold(KBD_DOWN);
			feed_key(KBD_DOWN);
			return;
		}
		if (c == 'C') {
			ser_hold(KBD_RIGHT);
			feed_key(KBD_RIGHT);
			return;
		}
		if (c == 'D') {
			ser_hold(KBD_LEFT);
			feed_key(KBD_LEFT);
			return;
		}
		return;
	}
	feed_key(c);
}

/** Parse `tetris 9` as a start level 0–19. */
static unsigned parse_level(int argc, char **argv)
{
	if (argc < 2) {
		return 0;
	}
	unsigned n = 0;
	const char *p = argv[1];
	if (*p == '\0') {
		return 0;
	}
	while (*p >= '0' && *p <= '9') {
		n = n * 10u + (unsigned)(*p - '0');
		p++;
		if (n > 19u) {
			return 19;
		}
	}
	return n;
}

/** Reset the well and RNG, spawn the first two pieces. */
static void game_init(unsigned start)
{
	memset(well, 0, sizeof(well));
	memset(clear_row, 0, sizeof(clear_row));
	start_level = start;
	level = start;
	lines = 0;
	next_level_at = first_goal(start);
	score = 0;
	das = 0;
	das_dir = 0;
	esc = 0;
	ser_left_until = 0;
	ser_right_until = 0;
	ser_down_until = 0;
	well_x = 2;
	well_y = 3;
	rng_state = (uint32_t)pit_ticks();
	if (rng_state == 0) {
		rng_state = 0xA5A5u;
	}
	last_kind = PK_I;
	nxt = roll_piece();
	spawn_piece();
	running = 1;
	dirty = 1;
}

void tetris_cmd(int argc, char **argv)
{
	if (argc > 1 && (strcmp(argv[1], "scores") == 0 || strcmp(argv[1], "score") == 0)) {
		scores_print();
		return;
	}
	unsigned start = parse_level(argc, argv);
	scores_load();
	tty_clear();
	game_init(start);
	paint();
	uint64_t next_frame = pit_ticks();
	while (running) {
		int c;
		while ((c = kbd_getc()) >= 0) {
			feed_key(c);
			audio_service();
		}
		while ((c = serial_getc()) >= 0) {
			feed_serial(c);
			audio_service();
		}
		uint64_t now = pit_ticks();
		if (now >= next_frame) {
			next_frame += FRAME_MS;
			if (now > next_frame + 80u) {
				next_frame = now + FRAME_MS;
			}
			switch (state) {
			case ST_PLAY:
				play_frame();
				break;
			case ST_ARE:
			case ST_CLEAR:
				wait_frame();
				break;
			case ST_PAUSE:
			case ST_OVER:
			case ST_NAME:
				break;
			}
		}
		if (dirty) {
			paint();
		}
		audio_service();
		__asm__ volatile ("pause");
	}
	tty_clear();
}

/*
 * game.c - the invaders' rules (game.h), a frame at a time, on the one-bit
 * screen. What's from the original (its disassembly) is said so; the rest
 * (where the text and the shields are, how long some explosions show) is
 * this one's, after how the game looks.
 */
#include <stdlib.h>
#include <string.h>
#include "game.h"

#define CELL			16		/* an invader's place in the rack, and its sprite's width (the original's) */
#define PLAYER_MIN		16		/* the cannon's left, as far as it goes (Xr 30, D9 - 32: the original's) */
#define PLAYER_MAX		185
#define PLAYER_START		16
#define SHOT_TOP		36		/* the cannon's shot explodes here, under the scores */
#define EDGE_LEFT		9		/* the rack turns when an invader's pixel is on these columns (the original's lines) */
#define EDGE_RIGHT		213
#define EDGE_SCAN_TOP		40		/* ... looked at between these rows */
#define EDGE_SCAN_BOTTOM	223
#define BLOW_FRAMES		16		/* the cannon's shot's explosion, an invader's (the original's 0x10) */
#define ASHOT_BLOW		12
#define SAUCER_BLOW		30
#define SAUCER_SHOW		60		/* its score shown */
#define PLAYER_BLAST_STEP	5		/* the cannon's explosion: a frame every 5, 12 of them (the original's) */
#define PLAYER_BLAST_STEPS	12
#define PLAYER_WAIT		48		/* then, before it's back */
#define SAUCER_EVERY		0x600		/* frames (the original's) */
#define FIRE_DELAY		0x30		/* the invaders hold their fire, a round's start, after a cannon's loss */
#define ROUND_WAIT		90		/* the last invader's gone: the next round after this */

static const int row_points[IV_ROWS] = {10, 10, 20, 20, 30};	/* (1DA0) */
static const int row_kind[IV_ROWS] = {0, 0, 1, 1, 2};
static const uint8_t round_y[8] = {0x60, 0x50, 0x48, 0x48, 0x48, 0x40, 0x40, 0x40};	/* (1DA3; the first round 0x78) */
static const uint8_t saucer_scores[15] = {10, 5, 5, 10, 15, 10, 10, 5, 30, 10, 10, 10, 5, 15, 10};	/* x 10 (1D54) */
static const uint8_t column_table[21] = {1, 7, 1, 1, 1, 4, 11, 1, 6, 3, 1, 1, 11, 9, 2, 8, 2, 11, 4, 7, 10};	/* (1D00) */
static const uint8_t fleet_count[16] = {50, 43, 36, 28, 22, 17, 13, 10, 8, 7, 6, 5, 4, 3, 2, 1};	/* (1A11) */
static const uint8_t fleet_delay[16] = {52, 46, 39, 34, 28, 24, 21, 19, 16, 14, 13, 12, 11, 9, 7, 5};	/* (1A21) */

/* ---- the screen --------------------------------------------------------------------------- */

static void pixel (iv_t *g, int x, int y, bool on)
{
	if (x < 0 || y < 0 || x >= IV_W || y >= IV_H)
	{
		return;
	}
	uint8_t v = on ? 255 : 0;
	if (g->screen[y][x] != v)
	{
		g->screen[y][x] = v;
		g->dirty[y] = 1;
	}
}

static bool lit (const iv_t *g, int x, int y)
{
	return x >= 0 && y >= 0 && x < IV_W && y < IV_H && g->screen[y][x];
}

enum { OR, CLEAR };

static void blit (iv_t *g, const iv_sprite_t *s, int x, int y, int mode)
{
	for (int r = 0; r < s->h; r++)
	{
		for (int c = 0; c < s->w; c++)
		{
			if (s->rows[r * s->w + c] == '#')
			{
				pixel (g, x + c, y + r, mode == OR);
			}
		}
	}
}

static void clear_rect (iv_t *g, int x, int y, int w, int h)
{
	for (int r = y; r < y + h; r++)
	{
		for (int c = x; c < x + w; c++)
		{
			pixel (g, c, r, false);
		}
	}
}

/* the first lit pixel in the rectangle, scanning rows from `from` (its top or bottom row) */
static bool find_lit (const iv_t *g, int x, int y, int w, int h, bool from_bottom, int *hx, int *hy)
{
	for (int i = 0; i < h; i++)
	{
		int r = from_bottom ? y + h - 1 - i : y + i;
		for (int c = x; c < x + w; c++)
		{
			if (lit (g, c, r))
			{
				*hx = c;
				*hy = r;
				return true;
			}
		}
	}
	return false;
}

static void text (iv_t *g, int x, int y, const char *s)
{
	for (; *s; s++, x += 8)
	{
		clear_rect (g, x, y, 8, 8);
		const iv_sprite_t *gl = iv_glyph (*s);
		if (gl)
		{
			blit (g, gl, x, y, OR);
		}
	}
}

static void number (iv_t *g, int x, int y, int n, int digits)
{
	char s[8];
	for (int i = digits - 1; i >= 0; i--, n /= 10)
	{
		s[i] = (char) ('0' + n % 10);
	}
	s[digits] = '\0';
	text (g, x, y, s);
}

void iv_clean (iv_t *g)
{
	memset (g->dirty, 0, sizeof g->dirty);
}

static void clear_screen (iv_t *g)
{
	memset (g->screen, 0, sizeof g->screen);
	memset (g->dirty, 1, sizeof g->dirty);
}

/* the scores at the top, the ground, the cannons left, the credit */
static void frame_text (iv_t *g)
{
	text (g, 8, 8, "SCORE<1> HI-SCORE SCORE<2>");
	number (g, 24, 24, g->score, 4);
	number (g, 88, 24, g->hi_score, 4);
	for (int x = 0; x < IV_W; x++)
	{
		pixel (g, x, IV_GROUND_Y, true);
	}
	text (g, 136, 241, "CREDIT 00");
}

static void lives_text (iv_t *g)
{
	clear_rect (g, 0, 241, 128, 8);
	number (g, 8, 241, g->lives, 1);
	for (int i = 0; i < g->lives - 1 && i < 6; i++)
	{
		blit (g, &iv_cannon, 24 + 16 * i, 241, OR);
	}
}

/* ---- the rack ----------------------------------------------------------------------------- */

static int kind_offset (int kind)	{ return (CELL - iv_invader[kind][0].w) / 2; }

/* an invader's cell written at (x, y): its sprite, the rest of the cell cleared (what was under
   it goes: the shields) */
static void draw_invader (iv_t *g, int row, int col, int x, int y)
{
	int kind = row_kind[row];
	clear_rect (g, g->inv_x[row][col], g->inv_y[row][col], CELL, 8);
	clear_rect (g, x, y, CELL, 8);
	blit (g, &iv_invader[kind][g->anim], x + kind_offset (kind), y, OR);
	g->inv_x[row][col] = x;
	g->inv_y[row][col] = y;
}

static void setup_round (iv_t *g)
{
	clear_screen (g);
	frame_text (g);
	lives_text (g);
	for (int i = 0; i < IV_SHIELDS; i++)
	{
		blit (g, &iv_shield, 32 + 45 * i, IV_SHIELD_Y, OR);
	}
	int yr = g->round == 0 ? 0x78 : round_y[(g->round - 1) % 8];
	g->ref_x = 0x38 - 32;			/* (the original's reference: Xr 38, Yr by the round) */
	g->ref_y = IV_H - yr - 8;
	for (int r = 0; r < IV_ROWS; r++)
	{
		for (int c = 0; c < IV_COLS; c++)
		{
			g->alive_inv[r][c] = true;
			g->inv_x[r][c] = g->ref_x + CELL * c;
			g->inv_y[r][c] = g->ref_y - CELL * r;
		}
	}
	g->n_alive = IV_ROWS * IV_COLS;
	g->dx = 2;
	g->drop = false;
	g->anim = false;
	g->cursor = 0;
	g->exploding = 0;
	memset (&g->shot, 0, sizeof g->shot);
	memset (g->ashot, 0, sizeof g->ashot);
	g->ashot[1].column = 0;
	g->ashot[2].column = 6;
	g->saucer = false;
	g->saucer_timer = 0;
	g->saucer_show = 0;
	g->fire_delay = FIRE_DELAY;
	g->fleet_timer = 0;
	g->px = PLAYER_START;
	g->alive = true;
	g->blast = 0;
	blit (g, &iv_cannon, g->px + 2, IV_PLAYER_Y, OR);
	g->state = IV_PLAYING;
	g->frame = 0;
}

void iv_new_game (iv_t *g, bool demo, int hi_score)
{
	uint32_t rng = g->rng ? g->rng : 0x1978u;
	memset (g, 0, sizeof *g);
	g->rng = rng;
	g->demo = demo;
	g->hi_score = hi_score;
	g->lives = 3;
	g->round = 0;
	setup_round (g);
	g->events |= IV_EV_ROUND;
}

void iv_attract (iv_t *g, int hi_score)
{
	uint32_t rng = g->rng ? g->rng : 0x1978u;
	memset (g, 0, sizeof *g);
	g->rng = rng;
	g->hi_score = hi_score;
	g->state = IV_ATTRACT;
	clear_screen (g);
	frame_text (g);
}

/* the next living invader moves (the rack's move first, when the turn comes round again) */
static void rack_step (iv_t *g)
{
	if (!g->n_alive)
	{
		return;
	}
	for (int tries = 0; tries <= IV_ROWS * IV_COLS; tries++)
	{
		if (g->cursor == 0)
		{
			g->ref_x += g->dx;		/* (MoveRefAlien: the delta, the frame) */
			if (g->drop)
			{
				g->ref_y += 8;
				g->drop = false;
			}
			g->anim = !g->anim;
		}
		int i = g->cursor;
		g->cursor = (g->cursor + 1) % (IV_ROWS * IV_COLS);
		int row = i / IV_COLS, col = i % IV_COLS;
		if (!g->alive_inv[row][col])
		{
			continue;
		}
		int x = g->ref_x + CELL * col, y = g->ref_y - CELL * row;
		draw_invader (g, row, col, x, y);
		if (y + 8 > IV_PLAYER_Y)
		{
			g->state = IV_PLAYER_HIT;	/* (they've landed: the game's over) */
			g->lives = 1;
			g->alive = false;
			g->blast = PLAYER_BLAST_STEP * PLAYER_BLAST_STEPS;
			g->frame = 0;
			g->events |= IV_EV_PLAYER_HIT;
		}
		return;
	}
}

/* at a side: an invader's pixel on the edge's column (RackBump) */
static void rack_bump (iv_t *g)
{
	int x = g->dx > 0 ? EDGE_RIGHT : EDGE_LEFT;
	for (int y = EDGE_SCAN_TOP; y <= EDGE_SCAN_BOTTOM; y++)
	{
		if (lit (g, x, y))
		{
			bool right = g->dx < 0;
			g->dx = right ? (g->n_alive == 1 ? 3 : 2) : -2;
			g->drop = true;
			return;
		}
	}
}

/* ---- hits --------------------------------------------------------------------------------- */

static void add_score (iv_t *g, int points)
{
	g->score = (g->score + points) % 10000;
	number (g, 24, 24, g->score, 4);
	if (!g->extra_given && g->score >= 1500)
	{
		g->extra_given = true;			/* (one, at 1500: the original's) */
		g->lives++;
		lives_text (g);
		g->events |= IV_EV_EXTRA;
	}
}

static bool invader_at (const iv_t *g, int x, int y, int *row, int *col)
{
	for (int r = 0; r < IV_ROWS; r++)
	{
		for (int c = 0; c < IV_COLS; c++)
		{
			if (g->alive_inv[r][c] && x >= g->inv_x[r][c] && x < g->inv_x[r][c] + CELL
			    && y >= g->inv_y[r][c] && y < g->inv_y[r][c] + 8)
			{
				*row = r;
				*col = c;
				return true;
			}
		}
	}
	return false;
}

static int ashot_at (const iv_t *g, int x, int y)
{
	for (int k = 0; k < 3; k++)
	{
		const iv_shot_t *s = &g->ashot[k];
		if (s->on && x >= s->x && x < s->x + 3 && y >= s->y && y < s->y + 8)
		{
			return k;
		}
	}
	return -1;
}

static void erase_ashot (iv_t *g, iv_shot_t *s)
{
	clear_rect (g, s->x, s->y, 3, 8);
}

static void player_hit (iv_t *g)
{
	g->alive = false;
	g->blast = PLAYER_BLAST_STEP * PLAYER_BLAST_STEPS;
	g->state = IV_PLAYER_HIT;
	g->frame = 0;
	g->events |= IV_EV_PLAYER_HIT;
	g->event_x = g->px + 8;
}

static void shot_blow (iv_t *g, int x, int y)
{
	clear_rect (g, g->shot.x, g->shot.y, 1, 4);
	g->shot.on = false;
	g->shot.blow = BLOW_FRAMES;
	g->shot.x = x - 3;			/* (the explosion's place) */
	g->shot.y = y - 4;
	blit (g, &iv_shot_blast, g->shot.x, g->shot.y, OR);
}

static void ashot_blow (iv_t *g, iv_shot_t *s, int x, int y)
{
	erase_ashot (g, s);
	s->on = false;
	s->blow = ASHOT_BLOW;
	s->x = x - 2;
	s->y = y - 4;
	blit (g, &iv_ashot_blast, s->x, s->y, OR);
}

/* ---- the cannon and its shot ----------------------------------------------------------------- */

static void player_step (iv_t *g, int dir, bool fire)
{
	if (g->alive && dir)
	{
		int nx = g->px + (dir > 0 ? 1 : -1);
		if (nx >= PLAYER_MIN && nx <= PLAYER_MAX)
		{
			clear_rect (g, g->px, IV_PLAYER_Y, CELL, 8);
			g->px = nx;
			blit (g, &iv_cannon, g->px + 2, IV_PLAYER_Y, OR);
		}
	}
	bool pressed = fire && !g->fire_held;
	g->fire_held = fire;
	if (pressed && g->alive && !g->shot.on && !g->shot.blow)
	{
		g->shot.on = true;
		g->shot.x = g->px + 8;			/* (the cannon's middle: the original's +8) */
		g->shot.y = IV_PLAYER_Y - 4;
		g->shot.steps = 0;
		g->shots_fired++;
		g->events |= IV_EV_SHOT;
		g->event_x = g->shot.x;
		for (int r = g->shot.y; r < g->shot.y + 4; r++)
		{
			pixel (g, g->shot.x, r, true);
		}
	}
}

static void saucer_hit (iv_t *g);

static void shot_step (iv_t *g)
{
	iv_shot_t *s = &g->shot;
	if (s->blow)
	{
		if (--s->blow == 0)
		{
			blit (g, &iv_shot_blast, s->x, s->y, CLEAR);	/* (a hole where it was: the shields') */
			g->saucer_pointer = (g->saucer_pointer + 1) % 15;	/* (the original wraps after 15) */
		}
		return;
	}
	if (!s->on)
	{
		return;
	}
	clear_rect (g, s->x, s->y, 1, 4);
	s->y -= 4;					/* (the original's shotDeltaX: 4) */
	s->steps++;
	if (s->y <= SHOT_TOP)
	{
		shot_blow (g, s->x, SHOT_TOP + 4);
		return;
	}
	int hx, hy;
	if (find_lit (g, s->x, s->y, 1, 4, true, &hx, &hy))
	{
		int row, col, k;
		if (g->saucer && hy >= IV_SAUCER_Y && hy < IV_SAUCER_Y + 8)
		{
			clear_rect (g, s->x, s->y, 1, 4);
			s->on = false;
			g->saucer_pointer = (g->saucer_pointer + 1) % 15;
			saucer_hit (g);
		}
		else if (!g->exploding && invader_at (g, hx, hy, &row, &col))
		{
			s->on = false;
			g->saucer_pointer = (g->saucer_pointer + 1) % 15;
			g->alive_inv[row][col] = false;
			g->n_alive--;
			clear_rect (g, g->inv_x[row][col], g->inv_y[row][col], CELL, 8);
			g->exp_x = g->inv_x[row][col] + 1;
			g->exp_y = g->inv_y[row][col];
			blit (g, &iv_invader_blast, g->exp_x, g->exp_y, OR);
			g->exploding = 1;
			g->exp_timer = BLOW_FRAMES;
			add_score (g, row_points[row]);
			g->events |= IV_EV_INVADER_HIT;
			g->event_x = hx;
		}
		else if ((k = ashot_at (g, hx, hy)) >= 0)
		{
			ashot_blow (g, &g->ashot[k], hx, hy);	/* (the two shots meet: both go) */
			shot_blow (g, hx, hy);
		}
		else
		{
			shot_blow (g, hx, hy);		/* (a shield, what's left of an explosion) */
		}
		return;
	}
	for (int r = s->y; r < s->y + 4; r++)
	{
		pixel (g, s->x, r, true);
	}
}

/* ---- the invaders' shots, the saucer -------------------------------------------------------- */

static int lowest_in_column (const iv_t *g, int col)
{
	for (int r = 0; r < IV_ROWS; r++)
	{
		if (g->alive_inv[r][col])
		{
			return r;
		}
	}
	return -1;
}

static int reload_rate (int score)
{
	int msb = score / 100;			/* (the score's high byte, BCD: 1CB8 and 1AA1) */
	return msb <= 2 ? 0x30 : msb <= 10 ? 0x10 : msb <= 20 ? 0x0B : msb <= 30 ? 0x08 : 0x07;
}

static void try_fire (iv_t *g, int k)
{
	if (g->fire_delay > 0 || !g->alive)
	{
		return;
	}
	for (int j = 0; j < 3; j++)			/* (the others must have gone far enough) */
	{
		if (j != k && g->ashot[j].on && g->ashot[j].steps && g->reload >= g->ashot[j].steps)
		{
			return;
		}
	}
	iv_shot_t *s = &g->ashot[k];
	int col;
	if (k == 0)					/* the rolling shot: over the cannon */
	{
		col = (g->px + 8 - g->ref_x) / CELL;
		if (col < 0 || col >= IV_COLS)
		{
			return;
		}
	}
	else
	{
		if (k == 1 && g->n_alive == 1)
		{
			return;				/* (no plunger for the last one) */
		}
		col = column_table[s->column] - 1;
		s->column = k == 1 ? (s->column + 1) % 16 : (s->column >= 20 ? 6 : s->column + 1);
	}
	int row = lowest_in_column (g, col);
	if (row < 0)
	{
		return;
	}
	s->on = true;
	s->x = g->inv_x[row][col] + CELL / 2 - 1;
	s->y = g->inv_y[row][col] + 8;
	s->steps = 0;
	s->frame = 0;
	blit (g, &iv_ashot[k][0], s->x, s->y, OR);
}

static void ashot_step (iv_t *g, int k)
{
	iv_shot_t *s = &g->ashot[k];
	if (s->blow)
	{
		if (--s->blow == 0)
		{
			blit (g, &iv_ashot_blast, s->x, s->y, CLEAR);
		}
		return;
	}
	if (!s->on)
	{
		try_fire (g, k);
		return;
	}
	erase_ashot (g, s);
	s->y += g->n_alive < 9 ? 5 : 4;			/* (alienShotDelta: FC, FB under 9: the original's) */
	s->steps++;
	s->frame = (s->frame + 1) % 4;
	if (s->y + 8 > IV_GROUND_Y)
	{
		ashot_blow (g, s, s->x + 1, IV_GROUND_Y - 3);	/* (it chips the ground) */
		return;
	}
	int hx, hy;
	if (find_lit (g, s->x, s->y, 3, 8, true, &hx, &hy))
	{
		if (g->alive && hy >= IV_PLAYER_Y && hx >= g->px && hx < g->px + CELL)
		{
			erase_ashot (g, s);
			s->on = false;
			player_hit (g);
		}
		else if (g->shot.on && hx == g->shot.x && hy >= g->shot.y && hy < g->shot.y + 4)
		{
			shot_blow (g, hx, hy);
			ashot_blow (g, s, hx, hy);
		}
		else
		{
			ashot_blow (g, s, hx, hy);
		}
		return;
	}
	blit (g, &iv_ashot[k][s->frame], s->x, s->y, OR);
}

static void saucer_hit (iv_t *g)
{
	clear_rect (g, g->saucer_x, IV_SAUCER_Y, CELL, 8);
	g->saucer = false;
	blit (g, &iv_saucer_blast, g->saucer_x - 2, IV_SAUCER_Y, OR);
	g->saucer_show = SAUCER_BLOW + SAUCER_SHOW;
	add_score (g, g->saucer_score);
	g->events |= IV_EV_SAUCER_HIT | IV_EV_SAUCER_OFF;
	g->event_x = g->saucer_x + 8;
}

static void saucer_step (iv_t *g)
{
	if (g->saucer_show)
	{
		g->saucer_show--;
		if (g->saucer_show == SAUCER_SHOW)	/* the explosion goes, its score shows */
		{
			clear_rect (g, g->saucer_x - 2, IV_SAUCER_Y, 24, 8);
			number (g, g->saucer_x, IV_SAUCER_Y, g->saucer_score, g->saucer_score >= 100 ? 3 : 2);
		}
		else if (g->saucer_show == 0)
		{
			clear_rect (g, g->saucer_x - 2, IV_SAUCER_Y, 32, 8);
		}
		return;
	}
	if (!g->saucer)
	{
		return;
	}
	clear_rect (g, g->saucer_x, IV_SAUCER_Y, CELL, 8);
	g->saucer_x += g->saucer_dx;
	if (g->saucer_x < 8 || g->saucer_x > IV_W - CELL - 8)
	{
		g->saucer = false;
		g->events |= IV_EV_SAUCER_OFF;
		return;
	}
	blit (g, &iv_saucer, g->saucer_x, IV_SAUCER_Y, OR);
}

/* ---- the frame ------------------------------------------------------------------------------- */

static void playing (iv_t *g, int dir, bool fire)
{
	/* an invader's explosion holds the rack still (the original's) */
	if (g->exploding && --g->exp_timer == 0)
	{
		clear_rect (g, g->exp_x, g->exp_y, 13, 8);
		g->exploding = 0;
	}
	player_step (g, dir, fire);
	shot_step (g);

	/* one of the three shots a frame: the squiggly (or the saucer), the plunger, the rolling */
	int k = 2 - g->shot_turn;
	g->shot_turn = (g->shot_turn + 1) % 3;
	if (k == 2 && (g->saucer || g->saucer_show))
	{
		saucer_step (g);
	}
	else if (g->state == IV_PLAYING)
	{
		ashot_step (g, k);
	}
	if (g->state != IV_PLAYING)
	{
		return;
	}
	if (!g->exploding)
	{
		rack_step (g);
		rack_bump (g);
	}
	if (g->state != IV_PLAYING)
	{
		return;
	}

	/* the saucer: when the rack's come down, every 0x600 frames, 8 invaders or more */
	if (g->ref_y > IV_H - 0x78 - 8)
	{
		g->saucer_timer++;
	}
	if (g->saucer_timer >= SAUCER_EVERY && !g->saucer && !g->saucer_show && !g->ashot[2].on && !g->ashot[2].blow
	    && g->n_alive >= 8)
	{
		g->saucer_timer = 0;
		g->saucer = true;
		bool right = g->shots_fired & 1;	/* (its way: the shot count's low bit) */
		g->saucer_x = right ? 0x29 - 32 : 0xE0 - 32;
		g->saucer_dx = right ? 2 : -2;
		g->saucer_score = saucer_scores[g->saucer_pointer] * 10;
		g->events |= IV_EV_SAUCER_ON;
	}

	/* the march: a note when its time's up, the time by the invaders left */
	if (--g->fleet_timer <= 0 && g->n_alive)
	{
		int i = 0;
		while (i < 15 && g->n_alive < fleet_count[i])
		{
			i++;
		}
		g->fleet_timer = fleet_delay[i];
		g->events |= IV_EV_FLEET;
		g->fleet_note = (g->fleet_note + 1) % 4;
	}
	g->reload = reload_rate (g->score);
	if (g->fire_delay > 0)
	{
		g->fire_delay--;
	}
	if (!g->n_alive && !g->exploding)
	{
		g->state = IV_ROUND_OVER;
		g->frame = 0;
	}
}

void iv_frame (iv_t *g, int dir, bool fire)
{
	g->events = 0;
	g->frame++;
	switch (g->state)
	{
	case IV_PLAYING:
		playing (g, dir, fire);
		break;
	case IV_PLAYER_HIT:
		if (g->blast > 0)
		{
			g->blast--;
			int step = g->blast / PLAYER_BLAST_STEP;
			clear_rect (g, g->px, IV_PLAYER_Y, CELL, 8);
			if (g->blast)
			{
				blit (g, &iv_cannon_blast[step & 1], g->px, IV_PLAYER_Y, OR);
			}
		}
		else if (g->frame >= PLAYER_BLAST_STEP * PLAYER_BLAST_STEPS + PLAYER_WAIT)
		{
			g->lives--;
			lives_text (g);
			for (int k = 0; k < 3; k++)
			{
				iv_shot_t *s = &g->ashot[k];
				if (s->on)
				{
					erase_ashot (g, s);
				}
				else if (s->blow)
				{
					blit (g, &iv_ashot_blast, s->x, s->y, CLEAR);
				}
				s->on = false;
				s->blow = 0;
			}
			if (g->lives <= 0 || g->demo)
			{
				g->state = IV_GAME_OVER;
				g->frame = 0;
				if (g->score > g->hi_score && !g->demo)
				{
					g->hi_score = g->score;
					number (g, 88, 24, g->hi_score, 4);
				}
				g->events |= IV_EV_GAME_OVER;
				break;
			}
			g->px = PLAYER_START;
			g->alive = true;
			g->fire_delay = FIRE_DELAY;
			blit (g, &iv_cannon, g->px + 2, IV_PLAYER_Y, OR);
			g->state = IV_PLAYING;
			g->frame = 0;
		}
		break;
	case IV_ROUND_OVER:
		if (g->shot.blow)
		{
			shot_step (g);
		}
		if (g->frame >= ROUND_WAIT)
		{
			g->round++;
			setup_round (g);
			g->events |= IV_EV_ROUND;
		}
		break;
	case IV_GAME_OVER:
		if (!g->demo && g->frame % 6 == 0 && g->frame / 6 <= 9)	/* (typed out, a letter at a time) */
		{
			char s[10] = "GAME OVER";
			s[g->frame / 6] = '\0';
			text (g, 76, 48, s);
		}
		break;
	case IV_ATTRACT:
	{
		/* the attract screen, written out as it goes */
		static const struct { int at, x, y; const char *s; } lines[] =
		{
			{20, 96, 64, "PLAY"}, {50, 80, 88, "INVADERS"}, {110, 32, 120, "*SCORE ADVANCE TABLE*"},
			{150, 80, 136, "=? MYSTERY"}, {180, 80, 152, "=30 POINTS"}, {210, 80, 168, "=20 POINTS"},
			{240, 80, 184, "=10 POINTS"}, {300, 72, 216, "PUSH START"}
		};
		for (unsigned i = 0; i < sizeof lines / sizeof lines[0]; i++)
		{
			int n = (g->frame - lines[i].at) / 4;	/* (a letter every 4 frames) */
			int len = (int) strlen (lines[i].s);
			if (n >= 0 && n <= len && (g->frame - lines[i].at) % 4 == 0)
			{
				char s[32];
				memcpy (s, lines[i].s, (size_t) n);
				s[n] = '\0';
				text (g, lines[i].x, lines[i].y, s);
			}
		}
		if (g->frame == 140)
		{
			blit (g, &iv_saucer, 60, 136, OR);
			blit (g, &iv_invader[2][0], 64 + kind_offset (2), 152, OR);
			blit (g, &iv_invader[1][0], 64 + kind_offset (1), 168, OR);
			blit (g, &iv_invader[0][0], 64 + kind_offset (0), 184, OR);
		}
		break;
	}
	}
}

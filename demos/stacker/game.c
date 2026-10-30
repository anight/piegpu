/*
 * game.c - stacker's rules (game.h) and its screen, a frame at a time
 */
#include <stdio.h>
#include <string.h>
#include "game.h"
#include "arcade_font.h"

#define FIELD_X		96		/* the field's top left on the screen (the NES's: tiles 12, 5) */
#define FIELD_Y		40
#define OPENING		96		/* the first piece waits this long (a soft drop ends it) */
#define DAS_FIRST	16
#define DAS_REPEAT	6
#define CURTAIN_STEP	4		/* the topped-out curtain: a row each (this one's) */

/* the ROM's orientation table: T up, right, down, left; J left, up, right, down; Z h, v; O; S h, v;
   L right, down, left, up; I v, h (x, y from the pivot, y down) */
const int8_t st_orient[19][4][2] =
{
	{{-1, 0}, {0, 0}, {1, 0}, {0, -1}}, {{0, -1}, {0, 0}, {1, 0}, {0, 1}},
	{{-1, 0}, {0, 0}, {1, 0}, {0, 1}}, {{0, -1}, {-1, 0}, {0, 0}, {0, 1}},
	{{0, -1}, {0, 0}, {-1, 1}, {0, 1}}, {{-1, -1}, {-1, 0}, {0, 0}, {1, 0}},
	{{0, -1}, {1, -1}, {0, 0}, {0, 1}}, {{-1, 0}, {0, 0}, {1, 0}, {1, 1}},
	{{-1, 0}, {0, 0}, {0, 1}, {1, 1}}, {{1, -1}, {0, 0}, {1, 0}, {0, 1}},
	{{-1, 0}, {0, 0}, {-1, 1}, {0, 1}},
	{{0, 0}, {1, 0}, {-1, 1}, {0, 1}}, {{0, -1}, {0, 0}, {1, 0}, {1, 1}},
	{{0, -1}, {0, 0}, {0, 1}, {1, 1}}, {{-1, 0}, {0, 0}, {1, 0}, {-1, 1}},
	{{-1, -1}, {0, -1}, {0, 0}, {0, 1}}, {{1, -1}, {-1, 0}, {0, 0}, {1, 0}},
	{{0, -2}, {0, -1}, {0, 0}, {0, 1}}, {{-2, 0}, {-1, 0}, {0, 0}, {1, 0}},
};
const uint8_t st_spawn[7] = {2, 7, 8, 10, 11, 14, 18};		/* T down, J down, Z h, O, S h, L down, I h */
static const uint8_t first_orient[7] = {0, 4, 8, 10, 11, 13, 17}, n_orient[7] = {4, 4, 2, 1, 2, 4, 2};

/* (this one's colours, after the NES's: two a level, ten levels in turn) */
const uint32_t st_level_colors[10][2] =
{
	{0x3CBCFC, 0x0058F8}, {0xB8F818, 0x00A800}, {0xF878F8, 0xD800CC}, {0x58D854, 0x0058F8},
	{0x58F898, 0xE40058}, {0x6888FC, 0x58F898}, {0xBCBCBC, 0xF83800}, {0xA80020, 0x6844FC},
	{0xF83800, 0x0000FC}, {0xFCA044, 0xF83800}
};

int st_piece_of (int orient)
{
	for (int p = 6; p >= 0; p--)
	{
		if (orient >= first_orient[p])
		{
			return p;
		}
	}
	return 0;
}

int st_rotate (int orient, bool clockwise)
{
	int p = st_piece_of (orient), k = orient - first_orient[p], n = n_orient[p];
	return first_orient[p] + (clockwise ? (k + 1) % n : (k + n - 1) % n);
}

int st_gravity (int level)
{
	static const uint8_t frames[10] = {48, 43, 38, 33, 28, 23, 18, 13, 8, 6};
	return level < 10 ? frames[level] : level < 13 ? 5 : level < 16 ? 4 : level < 19 ? 3 : level < 29 ? 2 : 1;
}

bool st_fits (const st_t *g, int orient, int px, int py)
{
	for (int k = 0; k < 4; k++)
	{
		int x = px + st_orient[orient][k][0], y = py + st_orient[orient][k][1];
		if (x < 0 || x >= ST_COLS || y >= ST_ROWS || (y >= 0 && g->field[y][x]))
		{
			return false;
		}
	}
	return true;
}

/* ---- the screen -------------------------------------------------------------------------- */

static void pixel (st_t *g, int x, int y, uint8_t v)
{
	if (x >= 0 && y >= 0 && x < ST_W && y < ST_H && g->screen[y][x] != v)
	{
		g->screen[y][x] = v;
		g->dirty[y] = 1;
	}
}

static void rect (st_t *g, int x, int y, int w, int h, uint8_t v)
{
	for (int r = y; r < y + h; r++)
	{
		for (int c = x; c < x + w; c++)
		{
			pixel (g, c, r, v);
		}
	}
}

static void text (st_t *g, int col, int row, const char *s, uint8_t color)
{
	for (int x = col * 8; *s; s++, x += 8)
	{
		rect (g, x, row * 8, 8, 8, C_BLACK);
		const char *rows = arcade_glyph (*s);
		for (int r = 0; rows && r < 7; r++)
		{
			for (int c = 0; c < 5; c++)
			{
				if (rows[r * 5 + c] == '#')
				{
					pixel (g, x + c + 1, row * 8 + r, color);
				}
			}
		}
	}
}

static void number (st_t *g, int col, int row, int n, int digits, uint8_t color)
{
	char s[12];
	for (int i = digits - 1; i >= 0; i--, n /= 10)
	{
		s[i] = (char) ('0' + n % 10);
	}
	s[digits] = '\0';
	text (g, col, row, s, color);
}

/* a block, 8 x 8 (7 and a gap), in its piece's kind: T O I white with the level's first colour round
   it; J S the second colour; Z L the first (after the NES's three) */
static void block (st_t *g, int x, int y, int piece)
{
	if (piece == P_NONE)
	{
		rect (g, x, y, 8, 8, C_FIELD);
		return;
	}
	bool hollow = piece == P_T || piece == P_O || piece == P_I;
	uint8_t c = piece == P_J || piece == P_S ? C_LEVEL2 : C_LEVEL1;
	for (int r = 0; r < 8; r++)
	{
		for (int k = 0; k < 8; k++)
		{
			uint8_t v = C_FIELD;
			if (r < 7 && k < 7)
			{
				bool rim = r == 0 || k == 0 || r == 6 || k == 6;
				v = hollow ? (rim ? c : C_WHITE) : c;
				if (!hollow && ((r == 1 && (k == 1 || k == 2)) || (r == 2 && k == 1)))
				{
					v = C_WHITE;		/* (the shine) */
				}
				if (hollow && r == 0 && k == 0)
				{
					v = C_WHITE;
				}
			}
			pixel (g, x + k, y + r, v);
		}
	}
}

/* a piece whole: the next one; smaller (blocks of 6) for the statistics */
static void piece_at (st_t *g, int x, int y, int piece)
{
	int o = st_spawn[piece];
	for (int k = 0; k < 4; k++)
	{
		block (g, x + st_orient[o][k][0] * 8, y + st_orient[o][k][1] * 8, piece);
	}
}

static void small_piece (st_t *g, int x, int y, int piece)
{
	int o = st_spawn[piece];
	bool hollow = piece == P_T || piece == P_O || piece == P_I;
	uint8_t c = piece == P_J || piece == P_S ? C_LEVEL2 : C_LEVEL1;
	for (int k = 0; k < 4; k++)
	{
		int bx = x + st_orient[o][k][0] * 6, by = y + st_orient[o][k][1] * 6;
		rect (g, bx, by, 5, 5, c);
		if (hollow)
		{
			rect (g, bx + 1, by + 1, 3, 3, C_WHITE);
		}
		else
		{
			pixel (g, bx + 1, by + 1, C_WHITE);
		}
	}
}

static void frame_box (st_t *g, int col, int row, int w, int h)
{
	rect (g, col * 8 - 3, row * 8 - 3, w * 8 + 6, h * 8 + 6, C_FRAME);
	rect (g, col * 8 - 1, row * 8 - 1, w * 8 + 2, h * 8 + 2, C_SHADE);
	rect (g, col * 8, row * 8, w * 8, h * 8, C_BLACK);
}

static void draw_frame (st_t *g)
{
	memset (g->screen, C_SHADE, sizeof g->screen);
	memset (g->dirty, 1, sizeof g->dirty);
	for (int y = 0; y < ST_H; y += 8)		/* (the backdrop: a quiet check) */
	{
		for (int x = 0; x < ST_W; x += 8)
		{
			rect (g, x, y, 8, 8, ((x + y) / 8) % 2 ? C_SHADE : C_BLACK);
		}
	}
	frame_box (g, 2, 2, 7, 1);
	text (g, 2, 2, "A-TYPE", C_WHITE);
	frame_box (g, 11, 1, 10, 1);
	frame_box (g, 23, 2, 7, 6);
	text (g, 23, 2, "TOP", C_WHITE);
	text (g, 23, 5, "SCORE", C_WHITE);
	frame_box (g, 23, 11, 6, 6);
	text (g, 23, 11, "NEXT", C_WHITE);
	frame_box (g, 23, 19, 6, 3);
	text (g, 23, 19, "LEVEL", C_WHITE);
	frame_box (g, 1, 6, 10, 19);
	text (g, 1, 6, "STATISTICS", C_WHITE);
	frame_box (g, FIELD_X / 8, FIELD_Y / 8, ST_COLS, ST_ROWS);
	rect (g, FIELD_X, FIELD_Y, ST_COLS * 8, ST_ROWS * 8, C_FIELD);
	for (int p = 0; p < 7; p++)
	{
		small_piece (g, 3 * 8, (8 + 2 * p) * 8 + 1, p);
	}
}

static void draw_numbers (st_t *g)
{
	char s[12];
	snprintf (s, sizeof s, "LINES-%03d", g->lines % 1000);
	text (g, 12, 1, s, C_WHITE);
	number (g, 23, 3, g->top_score % 1000000, 6, C_WHITE);
	number (g, 23, 6, g->score % 1000000, 6, C_WHITE);
	number (g, 25, 20, g->level, 2, C_WHITE);
	for (int p = 0; p < 7; p++)
	{
		number (g, 6, 8 + 2 * p, g->stats[p], 3, C_RED);
	}
}

static void draw_field (st_t *g)
{
	for (int y = 0; y < ST_ROWS; y++)
	{
		for (int x = 0; x < ST_COLS; x++)
		{
			block (g, FIELD_X + x * 8, FIELD_Y + y * 8, g->field[y][x] ? g->field[y][x] - 1 : P_NONE);
		}
	}
	if (g->state == ST_FALLING)
	{
		for (int k = 0; k < 4; k++)
		{
			int x = g->px + st_orient[g->orient][k][0], y = g->py + st_orient[g->orient][k][1];
			if (y >= 0)
			{
				block (g, FIELD_X + x * 8, FIELD_Y + y * 8, g->piece);
			}
		}
	}
	rect (g, 23 * 8, 12 * 8, 6 * 8, 5 * 8, C_BLACK);
	if (g->state != ST_TOPPED && g->state != ST_OVER)
	{
		piece_at (g, 25 * 8 + (g->next == P_I || g->next == P_O ? 4 : 0), 14 * 8, g->next);
	}
}

void st_clean (st_t *g)
{
	memset (g->dirty, 0, sizeof g->dirty);
}

/* ---- the pieces --------------------------------------------------------------------------- */

static void lfsr_step (st_t *g)
{
	/* (a 16-bit LFSR, as the NES's; its taps here: bits 1 and 9) */
	uint16_t bit = (uint16_t) (((g->lfsr >> 1) ^ (g->lfsr >> 9)) & 1);
	g->lfsr = (uint16_t) ((g->lfsr >> 1) | (bit << 15));
}

/* the next piece: a roll of 0 - 7, taken unless it's 7 or the last one; else a roll of 0 - 6 */
static int roll (st_t *g, int last)
{
	lfsr_step (g);
	int r = ((g->lfsr >> 8) + g->spawns) & 7;
	if (r != 7 && r != last)
	{
		return r;
	}
	lfsr_step (g);
	return (int) (((g->lfsr >> 8) & 7) + (unsigned) (last == P_NONE ? 0 : last)) % 7;
}

static void spawn (st_t *g)
{
	g->piece = g->next;
	g->next = roll (g, g->piece);
	g->orient = st_spawn[g->piece];
	g->px = 5;
	g->py = 0;
	g->gravity_t = 0;
	g->push_down = 0;
	g->spawns++;
	g->stats[g->piece]++;
	if (!st_fits (g, g->orient, g->px, g->py))
	{
		g->state = ST_TOPPED;
		g->frame = 0;
		g->events |= ST_EV_OVER;
		if (g->score > g->top_score && !g->demo)
		{
			g->top_score = g->score;
		}
		return;
	}
	g->state = ST_FALLING;
	g->frame = 0;
}

static int first_level_up (int start)
{
	int a = start * 10 + 10, b = start * 10 - 50 > 100 ? start * 10 - 50 : 100;
	return a < b ? a : b;
}

static void lock (st_t *g)
{
	int lowest = 0;
	for (int k = 0; k < 4; k++)
	{
		int x = g->px + st_orient[g->orient][k][0], y = g->py + st_orient[g->orient][k][1];
		if (y >= 0)
		{
			g->field[y][x] = (uint8_t) (g->piece + 1);
		}
		lowest = y > lowest ? y : lowest;
	}
	g->score += g->push_down;			/* (the last soft drop's rows) */
	g->push_down = 0;
	g->events |= ST_EV_LOCK;
	g->n_clear = 0;
	for (int y = 0; y < ST_ROWS; y++)
	{
		int n = 0;
		for (int x = 0; x < ST_COLS; x++)
		{
			n += g->field[y][x] != 0;
		}
		if (n == ST_COLS)
		{
			g->clear_rows[g->n_clear++] = y;
		}
	}
	int entry = 10 + 2 * ((ST_ROWS - 1 - lowest + 2) / 4);	/* (10 at the bottom, 2 more each 4 rows up) */
	g->entry_t = entry > 18 ? 18 : entry;
	if (g->n_clear)
	{
		g->state = ST_CLEARING;
		g->clear_step = 0;
	}
	else
	{
		g->state = ST_ENTRY;
	}
	g->frame = 0;
}

static void step_down (st_t *g)
{
	if (st_fits (g, g->orient, g->px, g->py + 1))
	{
		g->py++;
	}
	else
	{
		lock (g);
	}
}

static void falling (st_t *g, const st_input_t *in)
{
	/* across: once as it's pressed, again after 16 frames held, then every 6 (blocked: ready at once) */
	int dir = in->left && !in->right ? -1 : in->right && !in->left ? 1 : 0;
	bool pressed = (dir < 0 && !g->was_left) || (dir > 0 && !g->was_right);
	bool move = false;
	if (pressed)
	{
		move = true;
		g->das = 0;
	}
	else if (dir && ++g->das >= DAS_FIRST)
	{
		move = true;
		g->das = DAS_FIRST - DAS_REPEAT;
	}
	if (move)
	{
		if (st_fits (g, g->orient, g->px + dir, g->py))
		{
			g->px += dir;
			g->events |= ST_EV_MOVE;
			g->push_down = 0;
		}
		else
		{
			g->das = DAS_FIRST;
		}
	}
	/* turning, as it's pressed: where it is, or not at all */
	for (int k = 0; k < 2; k++)
	{
		bool now = k ? in->rotate_ccw : in->rotate_cw, was = k ? g->was_ccw : g->was_cw;
		if (now && !was)
		{
			int o = st_rotate (g->orient, k == 0);
			if (st_fits (g, o, g->px, g->py))
			{
				g->orient = o;
				g->events |= ST_EV_ROTATE;
			}
		}
	}
	/* down: the soft drop, a row in 2 frames; or the level's gravity */
	if (g->first)
	{
		if (in->down && !g->was_down)
		{
			g->first = false;
		}
		else if (g->frame < OPENING)
		{
			return;
		}
		g->first = false;
	}
	if (in->down && !dir)
	{
		if (!g->was_down)
		{
			g->down_t = 0;
		}
		if (++g->down_t >= 2)
		{
			g->down_t = 0;
			g->gravity_t = 0;
			g->push_down++;
			step_down (g);
			return;
		}
	}
	else if (!in->down)
	{
		g->push_down = 0;
	}
	if (++g->gravity_t >= st_gravity (g->level))
	{
		g->gravity_t = 0;
		step_down (g);
	}
}

static void clearing (st_t *g)
{
	if (g->frame % 4 == 0 && g->clear_step < 5)	/* (from the middle out, a column each side) */
	{
		for (int i = 0; i < g->n_clear; i++)
		{
			g->field[g->clear_rows[i]][4 - g->clear_step] = 0;
			g->field[g->clear_rows[i]][5 + g->clear_step] = 0;
		}
		g->clear_step++;
		return;
	}
	if (g->clear_step < 5)
	{
		return;
	}
	for (int i = 0; i < g->n_clear; i++)		/* the rows above come down */
	{
		for (int y = g->clear_rows[i]; y > 0; y--)
		{
			memcpy (g->field[y], g->field[y - 1], ST_COLS);
		}
		memset (g->field[0], 0, ST_COLS);
	}
	static const int points[5] = {0, 40, 100, 300, 1200};
	int was = g->level;
	g->lines += g->n_clear;
	int first = first_level_up (g->start_level);
	if (g->lines >= first)
	{
		g->level = g->start_level + 1 + (g->lines - first) / 10;
	}
	g->score += points[g->n_clear] * (g->level + 1);	/* (the level after the clear: the NES's) */
	g->events |= g->n_clear == 4 ? ST_EV_FOUR : ST_EV_LINES;
	if (g->level != was)
	{
		g->events |= ST_EV_LEVEL;
	}
	g->state = ST_ENTRY;
	g->frame = 0;
}

/* ---- the game ----------------------------------------------------------------------------- */

void st_title (st_t *g, int top_score, int start_level)
{
	uint16_t lfsr = g->lfsr ? g->lfsr : 0x8988;
	memset (g, 0, sizeof *g);
	g->lfsr = lfsr;
	g->top_score = top_score;
	g->start_level = start_level;
	g->state = ST_TITLE;
	memset (g->screen, C_BLACK, sizeof g->screen);
	memset (g->dirty, 1, sizeof g->dirty);
	frame_box (g, 8, 5, 16, 3);
	text (g, 9, 6, "S T A C K E R", C_WHITE);
	frame_box (g, 9, 12, 14, 3);
	text (g, 10, 13, "LEVEL", C_WHITE);
	text (g, 11, 20, "PUSH START", C_WHITE);
	char s[24];
	snprintf (s, sizeof s, "TOP %06d", top_score % 1000000);
	text (g, 11, 24, s, C_LEVEL1);
}

void st_new_game (st_t *g, bool demo, int start_level, int top_score)
{
	uint16_t lfsr = g->lfsr ? g->lfsr : 0x8988;
	memset (g, 0, sizeof *g);
	g->lfsr = lfsr;
	g->demo = demo;
	g->start_level = g->level = start_level;
	g->top_score = top_score;
	draw_frame (g);
	g->next = roll (g, P_NONE);
	spawn (g);
	g->first = true;
	draw_numbers (g);
	draw_field (g);
	g->events |= ST_EV_START;
}

void st_frame (st_t *g, const st_input_t *in)
{
	g->events = 0;
	g->frame++;
	int was_lines = g->lines, was_score = g->score, was_level = g->level, was_spawns = g->spawns;
	switch (g->state)
	{
	case ST_TITLE:
		if (g->frame % 4 == 0)
		{
			char s[8];
			snprintf (s, sizeof s, "< %d >", g->start_level);
			text (g, 16, 13, s, C_LEVEL1);
		}
		break;
	case ST_FALLING:
		falling (g, in);
		break;
	case ST_CLEARING:
		clearing (g);
		break;
	case ST_ENTRY:
		if (--g->entry_t <= 0)
		{
			spawn (g);
		}
		break;
	case ST_TOPPED:
		if (g->frame % CURTAIN_STEP == 0 && g->frame / CURTAIN_STEP <= ST_ROWS)	/* (the curtain comes down) */
		{
			int y = g->frame / CURTAIN_STEP - 1;
			for (int x = 0; x < ST_COLS && y >= 0; x++)
			{
				g->field[y][x] = P_I + 1;
			}
		}
		if (g->frame >= (ST_ROWS + 2) * CURTAIN_STEP)
		{
			g->state = ST_OVER;
			g->frame = 0;
		}
		break;
	case ST_OVER:
		if (g->frame == 1)
		{
			text (g, 13, 13, "GAME", C_WHITE);
			text (g, 13, 15, "OVER", C_WHITE);
		}
		break;
	}
	g->was_left = in->left;
	g->was_right = in->right;
	g->was_down = in->down;
	g->was_cw = in->rotate_cw;
	g->was_ccw = in->rotate_ccw;
	if (g->state == ST_TITLE)
	{
		return;
	}
	if (g->lines != was_lines || g->score != was_score || g->level != was_level || g->spawns != was_spawns)
	{
		draw_numbers (g);
	}
	if (g->state != ST_OVER)
	{
		draw_field (g);
	}
}

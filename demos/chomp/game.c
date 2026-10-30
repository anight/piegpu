/*
 * game.c - chomp's rules (game.h), a frame at a time. The tables are the
 * Dossier's (its Table A.1 and the text); what's this one's is said so.
 */
#include <string.h>
#include "game.h"
#include "arcade_font.h"

#define FULL_SPEED	75.75757625f	/* pixels a second at 100% (the Dossier's) */
#define HOUSE_SPEED	50		/* % in the house, going in and out (this one's) */
#define EYES_SPEED	200		/* % (this one's: the eyes are fast) */
#define READY_FRAMES	120
#define START_FRAMES	240		/* a game's first READY! */
#define EATEN_FRAMES	60		/* a ghost eaten: all stops this long, its score shown */
#define DIE_HOLD	60		/* caught: all stops, then the ghosts go */
#define DIE_FRAMES	88		/* the death's animation (11 pictures, 8 frames each) */
#define DIE_WAIT	60
#define FLASH_FRAMES	12		/* the maze's flashes at a level's end: 8 of these */
#define FRUIT_SHOW	120		/* its score shown */
#define FLASH_STEP	12		/* a frightened ghost's flash: white this long, then blue */
#define DOOR_X		112		/* the house's door, and above it */
#define DOOR_Y		116
#define HOUSE_Y		140
#define TUNNEL_ROW	17
#define FRIGHT_SEED	0x2A5u		/* the frightened ghosts' generator, each level and life (this one's) */

static const char *const maze[31] =
{
	"############################",
	"#............##............#",
	"#.####.#####.##.#####.####.#",
	"#o####.#####.##.#####.####o#",
	"#.####.#####.##.#####.####.#",
	"#..........................#",
	"#.####.##.########.##.####.#",
	"#.####.##.########.##.####.#",
	"#......##....##....##......#",
	"######.##### ## #####.######",
	"     #.##### ## #####.#     ",
	"     #.##          ##.#     ",
	"     #.## ###--### ##.#     ",
	"######.## #      # ##.######",
	"      .   #      #   .      ",
	"######.## #      # ##.######",
	"     #.## ######## ##.#     ",
	"     #.##          ##.#     ",
	"     #.## ######## ##.#     ",
	"######.## ######## ##.######",
	"#............##............#",
	"#.####.#####.##.#####.####.#",
	"#.####.#####.##.#####.####.#",
	"#o..##.......  .......##..o#",
	"###.##.##.########.##.##.###",
	"###.##.##.########.##.##.###",
	"#......##....##....##......#",
	"#.##########.##.##########.#",
	"#.##########.##.##########.#",
	"#..........................#",
	"############################",
};

/* ---- the tables -------------------------------------------------------------------------- */

typedef struct { uint8_t pac, ghost, tunnel, pac_fright, ghost_fright, elroy1_dots, elroy1, elroy2_dots, elroy2; } speeds_t;

static const speeds_t speeds_by[10] =			/* (Table A.1: %, and dots left for the red one's spurts) */
{
	{80, 75, 40, 90, 50, 20, 80, 10, 85},		/* 1 */
	{90, 85, 45, 95, 55, 30, 90, 15, 95},		/* 2 */
	{90, 85, 45, 95, 55, 40, 90, 20, 95},		/* 3-4 */
	{100, 95, 50, 100, 60, 40, 100, 20, 105},	/* 5 */
	{100, 95, 50, 100, 60, 50, 100, 25, 105},	/* 6-8 */
	{100, 95, 50, 100, 60, 60, 100, 30, 105},	/* 9-11 */
	{100, 95, 50, 100, 60, 80, 100, 40, 105},	/* 12-14 */
	{100, 95, 50, 100, 60, 100, 100, 50, 105},	/* 15-18 */
	{100, 95, 50, 100, 60, 120, 100, 60, 105},	/* 19-20 (no fright) */
	{90, 95, 50, 90, 60, 120, 100, 60, 105},	/* 21- */
};

static const speeds_t *speeds (int level)
{
	static const uint8_t index[22] = {0, 0, 1, 2, 2, 3, 4, 4, 4, 5, 5, 5, 6, 6, 6, 7, 7, 7, 7, 8, 8, 9};
	return &speeds_by[index[level < 21 ? level : 21]];
}

static const uint8_t fright_s[19] = {0, 6, 5, 4, 3, 2, 5, 2, 2, 1, 5, 2, 1, 1, 3, 1, 1, 0, 1};	/* by level; 19-: 0 */
static const uint8_t fright_flashes[19] = {0, 5, 5, 5, 5, 5, 5, 5, 5, 3, 5, 5, 3, 3, 5, 3, 3, 0, 3};

int ch_fright_frames (int level)	{ return level < 19 ? fright_s[level] * 60 : 0; }
int ch_fright_flashes (int level)	{ return level < 19 ? fright_flashes[level] : 0; }

/* scatter, chase, ... (frames; -1: for good) */
static const int phases[3][8] =
{
	{7 * 60, 20 * 60, 7 * 60, 20 * 60, 5 * 60, 20 * 60, 5 * 60, -1},	/* 1 */
	{7 * 60, 20 * 60, 7 * 60, 20 * 60, 5 * 60, 1033 * 60, 1, -1},		/* 2-4 */
	{5 * 60, 20 * 60, 5 * 60, 20 * 60, 5 * 60, 1037 * 60, 1, -1},		/* 5- */
};

static int phase_frames (int level, int phase)
{
	return phases[level == 1 ? 0 : level <= 4 ? 1 : 2][phase < 7 ? phase : 7];
}

int ch_fruit_kind (int level)
{
	static const uint8_t kind[14] = {0, 0, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7};
	return kind[level < 13 ? level : 13];
}

int ch_fruit_points (int level)
{
	static const int points[8] = {100, 300, 500, 700, 1000, 2000, 3000, 5000};
	return points[ch_fruit_kind (level)];
}

/* where they start; the scatter corners (tiles: as reimplementations have the Dossier's picture) */
static const int start_x[4] = {DOOR_X, DOOR_X, DOOR_X - 16, DOOR_X + 16};
static const int start_y[4] = {DOOR_Y, HOUSE_Y, HOUSE_Y, HOUSE_Y};
static const int corner_x[4] = {25, 2, 27, 0};
static const int corner_y[4] = {0, 0, 35, 35};

float ch_full_speed_px (void)		{ return FULL_SPEED / 60.0f; }

/* ---- the screen -------------------------------------------------------------------------- */

static void pixel (ch_t *g, int x, int y, uint8_t v)
{
	if (x >= 0 && y >= 0 && x < CH_W && y < CH_H && g->screen[y][x] != v)
	{
		g->screen[y][x] = v;
		g->dirty[y] = 1;
	}
}

static void rect (ch_t *g, int x, int y, int w, int h, uint8_t v)
{
	for (int r = y; r < y + h; r++)
	{
		for (int c = x; c < x + w; c++)
		{
			pixel (g, c, r, v);
		}
	}
}

/* text at a tile (cells of 8) */
static void text (ch_t *g, int col, int row, const char *s, uint8_t color)
{
	for (int x = col * 8; *s; s++, x += 8)
	{
		rect (g, x, row * 8, 8, 8, P_BLACK);
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

static void number (ch_t *g, int right_col, int row, int n, uint8_t color)
{
	char s[12];
	int i = 11;
	s[i] = '\0';
	do
	{
		s[--i] = (char) ('0' + n % 10);
		n /= 10;
	}
	while (n && i > 0);
	if (s[i] == '0' && s[i + 1] == '\0')
	{
		s[--i] = '0';			/* (no score: 00, as the arcade's) */
	}
	text (g, right_col - (10 - i), row, s + i, color);
}

void ch_clean (ch_t *g)
{
	memset (g->dirty, 0, sizeof g->dirty);
}

static bool wallish (const ch_t *g, int tx, int ty)
{
	if (tx < 0 || ty < 0 || tx >= CH_COLS || ty >= CH_ROWS)
	{
		return false;
	}
	return g->tile[ty][tx] == T_WALL || g->tile[ty][tx] == T_DOOR;
}

/* a wall tile: a line along each side that faces the open, 3 pixels in; the inner corners joined */
static void draw_wall (ch_t *g, int tx, int ty)
{
	int x = tx * 8, y = ty * 8;
	bool up = wallish (g, tx, ty - 1), down = wallish (g, tx, ty + 1);
	bool left = wallish (g, tx - 1, ty), right = wallish (g, tx + 1, ty);
	if (!up)
	{
		rect (g, x + (left ? 0 : 3), y + 3, (right ? 8 : 5) - (left ? 0 : 3), 1, P_WALL);
	}
	if (!down)
	{
		rect (g, x + (left ? 0 : 3), y + 4, (right ? 8 : 5) - (left ? 0 : 3), 1, P_WALL);
	}
	if (!left)
	{
		rect (g, x + 3, y + (up ? 0 : 3), 1, (down ? 8 : 5) - (up ? 0 : 3), P_WALL);
	}
	if (!right)
	{
		rect (g, x + 4, y + (up ? 0 : 3), 1, (down ? 8 : 5) - (up ? 0 : 3), P_WALL);
	}
	static const int8_t diag[4][2] = {{-1, -1}, {1, -1}, {-1, 1}, {1, 1}};
	for (int k = 0; k < 4; k++)
	{
		int dx = diag[k][0], dy = diag[k][1];
		if (wallish (g, tx + dx, ty) && wallish (g, tx, ty + dy) && !wallish (g, tx + dx, ty + dy))
		{
			int cx = x + (dx < 0 ? 3 : 4), cy = y + (dy < 0 ? 3 : 4);	/* (to the edges, from the corner) */
			rect (g, dx < 0 ? x : cx, cy, dx < 0 ? 4 : 4, 1, P_WALL);
			rect (g, cx, dy < 0 ? y : cy, 1, 4, P_WALL);
		}
	}
}

static void draw_tile (ch_t *g, int tx, int ty)
{
	int x = tx * 8, y = ty * 8;
	switch (g->tile[ty][tx])
	{
	case T_WALL:
		draw_wall (g, tx, ty);
		break;
	case T_DOOR:
		rect (g, x, y + 6, 8, 2, P_DOOR);
		break;
	case T_DOT:
		rect (g, x + 3, y + 3, 2, 2, P_DOT);
		break;
	case T_ENERGIZER:
		rect (g, x + 1, y, 6, 8, P_ENERGIZER);
		rect (g, x, y + 1, 8, 6, P_ENERGIZER);
		break;
	default:
		rect (g, x, y, 8, 8, P_BLACK);
		break;
	}
}

static void draw_scores (ch_t *g)
{
	text (g, 9, 0, "HIGH SCORE", P_WHITE);
	number (g, 6, 1, g->score, P_WHITE);
	if (g->hi_score)
	{
		number (g, 16, 1, g->hi_score, P_WHITE);
	}
}

static void setup_maze (ch_t *g)
{
	memset (g->tile, T_EMPTY, sizeof g->tile);
	for (int r = 0; r < 31; r++)
	{
		for (int c = 0; c < CH_COLS; c++)
		{
			char m = maze[r][c];
			g->tile[CH_MAZE_TOP + r][c] = m == '#' ? T_WALL : m == '-' ? T_DOOR : m == '.' ? T_DOT : m == 'o' ? T_ENERGIZER : T_EMPTY;
		}
	}
	memset (g->screen, P_BLACK, sizeof g->screen);
	memset (g->dirty, 1, sizeof g->dirty);
	for (int r = 0; r < CH_ROWS; r++)
	{
		for (int c = 0; c < CH_COLS; c++)
		{
			if (g->tile[r][c] != T_EMPTY)
			{
				draw_tile (g, c, r);
			}
		}
	}
	draw_scores (g);
	g->dots_eaten = 0;
}

bool ch_open (const ch_t *g, int tx, int ty, bool door)
{
	if (ty == TUNNEL_ROW && (tx < 0 || tx >= CH_COLS))
	{
		return true;
	}
	if (tx < 0 || ty < 0 || tx >= CH_COLS || ty >= CH_ROWS)
	{
		return false;
	}
	uint8_t t = g->tile[ty][tx];
	return t != T_WALL && (t != T_DOOR || door);
}

/* ---- starting ----------------------------------------------------------------------------- */

static int tile_of (int p)		{ return p >= 0 ? p / 8 : -((7 - p) / 8); }

static void place_actors (ch_t *g)
{
	ch_actor_t *p = &g->pac;
	memset (p, 0, sizeof *p);
	p->x = DOOR_X;				/* (13.5 tiles, the 26th row) */
	p->y = 26 * 8 + 4;
	p->dir = CH_LEFT;
	g->want = CH_NONE;
	g->pause = 0;
	g->mouth = 0;
	for (int i = 0; i < 4; i++)
	{
		ch_actor_t *gh = &g->ghost[i];
		int dots = gh->dots;
		memset (gh, 0, sizeof *gh);
		gh->dots = dots;
		gh->x = start_x[i];
		gh->y = start_y[i];
		gh->tx = tile_of (gh->x);
		gh->ty = tile_of (gh->y);
		gh->state = i == CH_RED ? G_ACTIVE : G_HOUSE;
		gh->dir = i == CH_RED ? CH_LEFT : i == CH_PINK ? CH_DOWN : CH_UP;
		gh->next = gh->plan = CH_LEFT;
	}
	g->phase = 0;
	g->phase_t = 0;
	g->fright_t = 0;
	g->idle_t = 0;
	g->fruit_t = 0;
	g->rng = FRIGHT_SEED;
}

static void start_level (ch_t *g)
{
	setup_maze (g);
	for (int i = 0; i < 4; i++)
	{
		g->ghost[i].dots = 0;
	}
	g->global_counter = false;
	g->elroy_held = false;
	g->fruit_shown = 0;
	place_actors (g);
	g->state = CH_READY;
	g->frame = 0;
	text (g, 11, 20, "READY!", P_YELLOW);
}

void ch_new_game (ch_t *g, bool demo, int hi_score)
{
	memset (g, 0, sizeof *g);
	g->demo = demo;
	g->hi_score = hi_score;
	g->level = 1;
	g->lives = 3;
	start_level (g);
	if (!demo)
	{
		text (g, 9, 14, "PLAYER ONE", P_CYAN);
		g->frame = READY_FRAMES - START_FRAMES;	/* (a longer READY! at the start) */
	}
	g->events |= CH_EV_READY;
}

void ch_attract (ch_t *g, int hi_score)
{
	memset (g, 0, sizeof *g);
	g->hi_score = hi_score;
	g->state = CH_ATTRACT;
	memset (g->screen, P_BLACK, sizeof g->screen);
	memset (g->dirty, 1, sizeof g->dirty);
	draw_scores (g);
}

/* ---- the ghosts --------------------------------------------------------------------------- */

static bool red_zone (int tx, int ty)		/* (no turning up here, in scatter and chase) */
{
	return (tx == 12 || tx == 15) && (ty == 14 || ty == 26);
}

static int dots_left (const ch_t *g)		{ return CH_DOTS - g->dots_eaten; }

static bool elroy (const ch_t *g, int *pct)
{
	const speeds_t *s = speeds (g->level);
	if (g->elroy_held)
	{
		return false;
	}
	if (dots_left (g) <= s->elroy2_dots)
	{
		*pct = s->elroy2;
		return true;
	}
	if (dots_left (g) <= s->elroy1_dots)
	{
		*pct = s->elroy1;
		return true;
	}
	return false;
}

static void target (ch_t *g, int i, int *tx, int *ty)
{
	ch_actor_t *gh = &g->ghost[i];
	const ch_actor_t *p = &g->pac;
	int px = tile_of (p->x), py = tile_of (p->y), pct;
	if (gh->state == G_EYES)
	{
		*tx = 13;
		*ty = 14;
		return;
	}
	bool scatter = !(g->phase & 1) && !(i == CH_RED && elroy (g, &pct));	/* (the spurting red one chases on) */
	if (scatter)
	{
		*tx = corner_x[i];
		*ty = corner_y[i];
		return;
	}
	int ax = ch_dx (p->dir), ay = ch_dy (p->dir);
	switch (i)
	{
	case CH_RED:
		*tx = px, *ty = py;
		break;
	case CH_PINK:					/* 4 ahead (up: 4 to the left too: the original's overflow) */
		*tx = px + 4 * ax - (p->dir == CH_UP ? 4 : 0);
		*ty = py + 4 * ay;
		break;
	case CH_CYAN:
	{
		int mx = px + 2 * ax - (p->dir == CH_UP ? 2 : 0), my = py + 2 * ay;
		int rx = tile_of (g->ghost[CH_RED].x), ry = tile_of (g->ghost[CH_RED].y);
		*tx = 2 * mx - rx;
		*ty = 2 * my - ry;
		break;
	}
	default:					/* orange: 8 tiles off or more, after it; nearer, its corner */
	{
		int dx = tile_of (gh->x) - px, dy = tile_of (gh->y) - py;
		if (dx * dx + dy * dy >= 64)
		{
			*tx = px, *ty = py;
		}
		else
		{
			*tx = corner_x[i], *ty = corner_y[i];
		}
	}
	}
}

static uint32_t next_random (ch_t *g)
{
	g->rng = g->rng * 1103515245u + 12345u;
	return g->rng >> 16;
}

/* the way it'll go at tile (nx, ny), coming in going d */
static int choose (ch_t *g, int i, int nx, int ny, int d)
{
	ch_actor_t *gh = &g->ghost[i];
	if (gh->fright && gh->state == G_ACTIVE)
	{
		static const int clockwise[4] = {CH_UP, CH_RIGHT, CH_DOWN, CH_LEFT};
		int k = (int) (next_random (g) & 3);
		for (int n = 0; n < 4; n++, k = (k + 1) % 4)
		{
			int c = clockwise[k];
			if (c != ch_back (d) && ch_open (g, nx + ch_dx (c), ny + ch_dy (c), false))
			{
				return c;
			}
		}
		return ch_back (d);
	}
	int tx, ty;
	target (g, i, &tx, &ty);
	gh->tgx = tx;
	gh->tgy = ty;
	int best = CH_NONE, best_d = 0x7FFFFFFF;
	for (int c = CH_UP; c <= CH_RIGHT; c++)		/* (ties: up, left, down, right) */
	{
		if (c == ch_back (d) || !ch_open (g, nx + ch_dx (c), ny + ch_dy (c), false))
		{
			continue;
		}
		if (c == CH_UP && gh->state == G_ACTIVE && red_zone (nx, ny))
		{
			continue;
		}
		int dx = nx + ch_dx (c) - tx, dy = ny + ch_dy (c) - ty, dd = dx * dx + dy * dy;
		if (dd < best_d)
		{
			best_d = dd;
			best = c;
		}
	}
	return best == CH_NONE ? ch_back (d) : best;
}

static void reverse_all (ch_t *g)
{
	for (int i = 0; i < 4; i++)
	{
		ch_actor_t *gh = &g->ghost[i];
		if (gh->state == G_ACTIVE)
		{
			gh->next = ch_back (gh->dir);	/* (it turns round at this tile's middle) */
			gh->plan = choose (g, i, gh->tx + ch_dx (gh->next), gh->ty + ch_dy (gh->next), gh->next);
		}
	}
}

static int ghost_speed (ch_t *g, int i)
{
	ch_actor_t *gh = &g->ghost[i];
	const speeds_t *s = speeds (g->level);
	int pct = s->ghost;
	if (gh->state == G_EYES)
	{
		return EYES_SPEED;
	}
	if (gh->state != G_ACTIVE)
	{
		return HOUSE_SPEED;
	}
	if (gh->ty == TUNNEL_ROW && (gh->tx <= 5 || gh->tx >= 22))
	{
		return s->tunnel;
	}
	if (gh->fright)
	{
		return s->ghost_fright;
	}
	if (i == CH_RED)
	{
		elroy (g, &pct);
	}
	return pct;
}

static void wrap (ch_actor_t *a)
{
	if (a->x < -8)
	{
		a->x += CH_W + 16;
	}
	else if (a->x >= CH_W + 8)
	{
		a->x -= CH_W + 16;
	}
}

/* a pixel on, along the lanes */
static void ghost_pixel (ch_t *g, int i)
{
	ch_actor_t *gh = &g->ghost[i];
	switch (gh->state)
	{
	case G_HOUSE:					/* bobbing */
		if (gh->y <= HOUSE_Y - 4)
		{
			gh->dir = CH_DOWN;
		}
		else if (gh->y >= HOUSE_Y + 4)
		{
			gh->dir = CH_UP;
		}
		gh->y += ch_dy (gh->dir);
		return;
	case G_LEAVING:
		if (gh->x != DOOR_X)
		{
			if (gh->y != HOUSE_Y)
			{
				gh->dir = gh->y < HOUSE_Y ? CH_DOWN : CH_UP;
				gh->y += ch_dy (gh->dir);
				return;
			}
			gh->dir = gh->x < DOOR_X ? CH_RIGHT : CH_LEFT;
			gh->x += ch_dx (gh->dir);
			return;
		}
		gh->dir = CH_UP;
		gh->y--;
		if (gh->y == DOOR_Y)
		{
			gh->state = G_ACTIVE;
			gh->dir = gh->next = CH_LEFT;
			gh->tx = tile_of (gh->x);
			gh->ty = tile_of (gh->y);
			gh->plan = choose (g, i, gh->tx - 1, gh->ty, CH_LEFT);
		}
		return;
	case G_ENTERING:
		if (gh->x != DOOR_X)
		{
			gh->dir = gh->x < DOOR_X ? CH_RIGHT : CH_LEFT;
			gh->x += ch_dx (gh->dir);
			return;
		}
		gh->dir = CH_DOWN;
		gh->y++;
		if (gh->y == HOUSE_Y)
		{
			gh->state = G_LEAVING;		/* (back: it comes straight out again) */
			gh->fright = false;
		}
		return;
	}

	/* along the lanes: at a tile's middle it turns as decided; coming into a tile it decides
	   for the next one */
	int cx = gh->tx * 8 + 4, cy = gh->ty * 8 + 4;
	if (gh->x == cx && gh->y == cy)
	{
		if (gh->state == G_EYES && gh->ty == 14 && (gh->tx == 13 || gh->tx == 14))
		{
			gh->state = G_ENTERING;		/* (home) */
			return;
		}
		gh->dir = gh->next;
		gh->next = gh->plan;
	}
	gh->x += ch_dx (gh->dir);
	gh->y += ch_dy (gh->dir);
	wrap (gh);
	int tx = tile_of (gh->x), ty = tile_of (gh->y);
	if (tx != gh->tx || ty != gh->ty)
	{
		gh->tx = tx;
		gh->ty = ty;
		gh->plan = choose (g, i, tx + ch_dx (gh->next), ty + ch_dy (gh->next), gh->next);
	}
}

static void ghosts_step (ch_t *g)
{
	for (int i = 0; i < 4; i++)
	{
		ch_actor_t *gh = &g->ghost[i];
		gh->acc += ghost_speed (g, i) / 100.0f * ch_full_speed_px ();
		while (gh->acc >= 1.0f)
		{
			gh->acc -= 1.0f;
			ghost_pixel (g, i);
		}
	}
}

/* the house: who comes out next (its counters, the global one after a life lost, the timer) */
static void release (ch_t *g, bool dot)
{
	static const int order[3] = {CH_PINK, CH_CYAN, CH_ORANGE};
	int limit[4] = {0, 0, g->level == 1 ? 30 : 0, g->level == 1 ? 60 : g->level == 2 ? 50 : 0};
	int preferred = -1;
	for (int k = 0; k < 3; k++)
	{
		if (g->ghost[order[k]].state == G_HOUSE)
		{
			preferred = order[k];
			break;
		}
	}
	if (dot)
	{
		g->idle_t = 0;
		if (g->global_counter)
		{
			g->global_dots++;
		}
		else if (preferred >= 0)
		{
			g->ghost[preferred].dots++;
		}
	}
	if (preferred < 0)
	{
		return;
	}
	bool go = false;
	if (g->global_counter)
	{
		static const int at[4] = {0, 7, 17, 32};
		if (g->global_dots >= at[preferred] && preferred != CH_ORANGE)
		{
			go = true;
		}
		else if (preferred == CH_ORANGE && g->global_dots >= 32)
		{
			g->global_counter = false;	/* (the orange one's in: back to their own counters) */
			go = true;
		}
	}
	else if (g->ghost[preferred].dots >= limit[preferred])
	{
		go = true;
	}
	if (g->idle_t >= (g->level < 5 ? 4 : 3) * 60)
	{
		g->idle_t = 0;
		go = true;
	}
	if (go)
	{
		g->ghost[preferred].state = G_LEAVING;
		if (preferred == CH_ORANGE)
		{
			g->elroy_held = false;
		}
	}
}

/* ---- the player --------------------------------------------------------------------------- */

static void pac_pixel (ch_t *g)
{
	ch_actor_t *p = &g->pac;
	int tx = tile_of (p->x), ty = tile_of (p->y), cx = tx * 8 + 4, cy = ty * 8 + 4;
	int w = g->want;
	if (w != CH_NONE && w != p->dir)
	{
		if (w == ch_back (p->dir) || ch_open (g, tx + ch_dx (w), ty + ch_dy (w), false))
		{
			p->dir = w;			/* (turned early or late: it cuts the corner) */
		}
	}
	int dx = ch_dx (p->dir), dy = ch_dy (p->dir);
	bool at_or_past = dx > 0 ? p->x >= cx : dx < 0 ? p->x <= cx : dy > 0 ? p->y >= cy : p->y <= cy;
	if (at_or_past && !ch_open (g, tx + dx, ty + dy, false))
	{
		if (dx)
		{
			p->x = cx;
		}
		else
		{
			p->y = cy;
		}
		if (dx && p->y != cy)
		{
			p->y += p->y < cy ? 1 : -1;
		}
		if (dy && p->x != cx)
		{
			p->x += p->x < cx ? 1 : -1;
		}
		return;
	}
	p->x += dx;
	p->y += dy;
	if (dx && p->y != cy)				/* the corner cut: a pixel across for one along */
	{
		p->y += p->y < cy ? 1 : -1;
	}
	if (dy && p->x != cx)
	{
		p->x += p->x < cx ? 1 : -1;
	}
	wrap (p);
	g->mouth++;
}

static void add_score (ch_t *g, int points)
{
	g->score += points;
	number (g, 6, 1, g->score, P_WHITE);
	if (!g->extra_given && g->score >= 10000)	/* (the arcade's usual setting) */
	{
		g->extra_given = true;
		g->lives++;
		g->events |= CH_EV_EXTRA;
	}
}

static void eat (ch_t *g)
{
	ch_actor_t *p = &g->pac;
	int tx = tile_of (p->x), ty = tile_of (p->y);
	if (tx < 0 || tx >= CH_COLS)
	{
		return;
	}
	uint8_t t = g->tile[ty][tx];
	if (t == T_DOT || t == T_ENERGIZER)
	{
		g->tile[ty][tx] = T_EMPTY;
		draw_tile (g, tx, ty);
		g->dots_eaten++;
		release (g, true);
		if (t == T_DOT)
		{
			add_score (g, 10);
			g->pause = 1;			/* (a frame a dot: the Dossier's) */
			g->events |= CH_EV_DOT;
		}
		else
		{
			add_score (g, 50);
			g->pause = 3;
			g->events |= CH_EV_ENERGIZER;
			g->eaten_in_fright = 0;
			reverse_all (g);
			g->fright_t = ch_fright_frames (g->level);
			for (int i = 0; i < 4 && g->fright_t; i++)
			{
				if (g->ghost[i].state != G_EYES && g->ghost[i].state != G_ENTERING)
				{
					g->ghost[i].fright = true;
				}
			}
		}
		if (g->dots_eaten == 70 || g->dots_eaten == 170)
		{
			g->fruit_t = 560 + (int) (next_random (g) % 41);	/* (9.33 - 10 s) */
		}
	}
	if (g->fruit_t && ty == 20 && (tx == 13 || tx == 14))
	{
		g->fruit_t = 0;
		g->fruit_score_t = FRUIT_SHOW;
		add_score (g, ch_fruit_points (g->level));
		g->events |= CH_EV_FRUIT;
	}
}

/* Pac and the ghosts in the same tile: one of them goes */
static void meet (ch_t *g)
{
	int px = tile_of (g->pac.x), py = tile_of (g->pac.y);
	for (int i = 0; i < 4; i++)
	{
		ch_actor_t *gh = &g->ghost[i];
		if (gh->state != G_ACTIVE || tile_of (gh->x) != px || tile_of (gh->y) != py)
		{
			continue;
		}
		if (gh->fright)
		{
			gh->fright = false;
			gh->state = G_EYES;		/* (its turn at this tile stays; from there, home) */
			gh->plan = choose (g, i, gh->tx + ch_dx (gh->next), gh->ty + ch_dy (gh->next), gh->next);
			int points = 200 << (g->eaten_in_fright < 3 ? g->eaten_in_fright : 3);
			g->eaten_in_fright++;
			add_score (g, points);
			g->popup_x = gh->x;
			g->popup_y = gh->y;
			g->popup_value = points;
			g->eaten_ghost = i;
			g->state = CH_EATEN;
			g->frame = 0;
			g->events |= CH_EV_GHOST;
			return;
		}
		g->state = CH_DYING;
		g->frame = 0;
		g->events |= CH_EV_DIE;
		return;
	}
}

/* ---- the frame ---------------------------------------------------------------------------- */

static void playing (ch_t *g, int want)
{
	g->want = want;

	/* scatter and chase (held while they're frightened) */
	if (g->fright_t)
	{
		if (--g->fright_t == 0)
		{
			for (int i = 0; i < 4; i++)
			{
				g->ghost[i].fright = false;
			}
		}
	}
	else
	{
		int n = phase_frames (g->level, g->phase);
		if (n >= 0 && ++g->phase_t >= n)
		{
			g->phase++;
			g->phase_t = 0;
			reverse_all (g);
		}
	}

	const speeds_t *s = speeds (g->level);
	if (g->pause)
	{
		g->pause--;
	}
	else
	{
		g->pac.acc += (g->fright_t ? s->pac_fright : s->pac) / 100.0f * ch_full_speed_px ();
		while (g->pac.acc >= 1.0f)
		{
			g->pac.acc -= 1.0f;
			pac_pixel (g);
			eat (g);
			if (g->pause)
			{
				g->pac.acc = 0;
				break;
			}
		}
	}
	meet (g);
	if (g->state != CH_PLAYING)
	{
		return;
	}
	ghosts_step (g);
	meet (g);
	if (g->state != CH_PLAYING)
	{
		return;
	}
	g->idle_t++;
	release (g, false);
	if (g->fruit_t && --g->fruit_t == 0)
	{
		g->events |= 0;				/* (gone, uneaten) */
	}
	if (g->fruit_score_t)
	{
		g->fruit_score_t--;
	}
	if (g->dots_eaten == CH_DOTS)
	{
		g->state = CH_LEVEL_DONE;
		g->frame = 0;
		g->fruit_t = 0;
	}
}

void ch_frame (ch_t *g, int want)
{
	g->events = 0;
	g->frame++;
	switch (g->state)
	{
	case CH_READY:
		if (g->frame >= READY_FRAMES)
		{
			text (g, 11, 20, "      ", P_BLACK);
			text (g, 9, 14, "          ", P_BLACK);
			g->state = CH_PLAYING;
			g->frame = 0;
		}
		break;
	case CH_PLAYING:
		playing (g, want);
		break;
	case CH_EATEN:
		if (g->frame >= EATEN_FRAMES)
		{
			g->state = CH_PLAYING;
			g->frame = 0;
		}
		break;
	case CH_DYING:
		if (g->frame >= DIE_HOLD + DIE_FRAMES + DIE_WAIT)
		{
			g->lives--;
			if (g->lives <= 0)
			{
				g->state = CH_GAME_OVER;
				g->frame = 0;
				text (g, 9, 20, "GAME  OVER", P_RED);
				if (g->score > g->hi_score && !g->demo)
				{
					g->hi_score = g->score;
					draw_scores (g);
				}
				g->events |= CH_EV_GAME_OVER;
				break;
			}
			g->global_counter = true;	/* (after a life lost: the global counter, the red one held back) */
			g->global_dots = 0;
			g->elroy_held = true;
			place_actors (g);
			g->state = CH_READY;
			g->frame = 0;
			text (g, 11, 20, "READY!", P_YELLOW);
			g->events |= CH_EV_READY;
		}
		break;
	case CH_LEVEL_DONE:
		if (g->frame >= 60 + 8 * FLASH_FRAMES)
		{
			g->level++;
			start_level (g);
			g->events |= CH_EV_LEVEL | CH_EV_READY;
		}
		break;
	case CH_ATTRACT:
	{
		/* the attract screen, written out as it goes (its ghosts drawn over it: chomp.c) */
		static const struct { int at, col, row; uint8_t color; const char *s; } lines[] =
		{
			{20, 11, 5, P_YELLOW, "CHOMP"}, {60, 8, 8, P_RED, "- RED"}, {100, 8, 11, P_PINK, "- PINK"},
			{140, 8, 14, P_CYAN, "- CYAN"}, {180, 8, 17, P_ORANGE, "- ORANGE"}, {230, 12, 24, P_WHITE, "10 PTS"},
			{230, 12, 26, P_WHITE, "50 PTS"}, {290, 9, 31, P_PEACH, "PUSH START"}
		};
		for (unsigned i = 0; i < sizeof lines / sizeof lines[0]; i++)
		{
			if (g->frame == lines[i].at)
			{
				text (g, lines[i].col, lines[i].row, lines[i].s, lines[i].color);
			}
		}
		if (g->frame == 230)
		{
			rect (g, 10 * 8 + 3, 24 * 8 + 3, 2, 2, P_DOT);
			rect (g, 10 * 8 + 1, 26 * 8, 6, 8, P_ENERGIZER);
			rect (g, 10 * 8, 26 * 8 + 1, 8, 6, P_ENERGIZER);
		}
		break;
	}
	case CH_GAME_OVER:
		break;
	}
}

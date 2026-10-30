/*
 * game.c - rockfall's rules (game.h): the cave scan, as the BoulderDash
 * Common File Format's object specifications give it. What's this one's
 * (not documented there) is said so.
 */
#include <string.h>
#include "game.h"

#define UNCOVER_FRAMES	69		/* the uncovering's loops: a cell a row each (the original's 69) */
#define INBOX_SCANS	24		/* the inbox flashes this many scans, then the miner's there (this one's) */
#define AMOEBA_TOO_BIG	200		/* (the C64's) */
#define DEAD_FRAMES	180		/* gone: the cave again after this, or fire (this one's) */
#define FLASH_FRAMES	6		/* the exit's out: the screen flashes (this one's length) */
#define LIVES		3

static const int8_t dir_dx[4] = {-1, 0, 1, 0}, dir_dy[4] = {0, -1, 0, 1};	/* left, up, right, down */

int rf_scan_frames (int difficulty)
{
	static const int frames[5] = {8, 7, 6, 5, 5};	/* (this one's: the original's varied with the cave) */
	return frames[(difficulty < 1 ? 1 : difficulty > 5 ? 5 : difficulty) - 1];
}

static uint32_t rnd (rf_t *g)
{
	g->rng ^= g->rng << 13;
	g->rng ^= g->rng >> 17;
	g->rng ^= g->rng << 5;
	return g->rng;
}

static int at (const rf_t *g, int x, int y)
{
	if (x < 0 || y < 0 || x >= RF_W || y >= RF_H)
	{
		return O_STEEL;
	}
	return rf_object (g->cell[y][x]);
}

static void set (rf_t *g, int x, int y, uint16_t v)
{
	if (x >= 0 && y >= 0 && x < RF_W && y < RF_H)
	{
		g->cell[y][x] = v;
	}
}

/* moved to (x, y) from (fx, fy): marked if that's ahead in the scan, so it isn't moved again */
static void move (rf_t *g, int fx, int fy, int x, int y, uint16_t v)
{
	set (g, fx, fy, O_SPACE);
	bool ahead = y > fy || (y == fy && x > fx);
	set (g, x, y, (uint16_t) (v | (ahead ? F_SCANNED : 0)));
}

/* ---- explosions ----------------------------------------------------------------------------- */

static void explode (rf_t *g, int cx, int cy, bool diamonds, int sx, int sy)
{
	g->events |= RF_EV_EXPLODE;
	for (int y = cy - 1; y <= cy + 1; y++)
	{
		for (int x = cx - 1; x <= cx + 1; x++)
		{
			if (at (g, x, y) == O_STEEL)
			{
				continue;		/* (steel stays) */
			}
			if (at (g, x, y) == O_MINER)
			{
				g->events |= RF_EV_DIE;
			}
			/* cells still to be scanned start at stage 0, so they're all at 1 after the scan */
			bool scanned = y < sy || (y == sy && x <= sx);
			set (g, x, y, (uint16_t) ((diamonds ? O_EXPLODE_DIAMOND : O_EXPLODE) | ((scanned ? 1 : 0) << F_DIR_SHIFT)));
		}
	}
}

/* ---- boulders and diamonds ------------------------------------------------------------------- */

static bool rounded (uint16_t c)
{
	int o = rf_object (c);
	return o == O_BRICK || ((o == O_BOULDER || o == O_DIAMOND) && !(c & F_FALLING));
}

static bool roll (rf_t *g, int x, int y, uint16_t v)
{
	if (at (g, x - 1, y) == O_SPACE && at (g, x - 1, y + 1) == O_SPACE)	/* (left first) */
	{
		move (g, x, y, x - 1, y, (uint16_t) (v | F_FALLING));
		return true;
	}
	if (at (g, x + 1, y) == O_SPACE && at (g, x + 1, y + 1) == O_SPACE)
	{
		move (g, x, y, x + 1, y, (uint16_t) (v | F_FALLING));
		return true;
	}
	return false;
}

static void falling_thing (rf_t *g, int x, int y)
{
	uint16_t c = g->cell[y][x];
	int o = rf_object (c);
	uint16_t sound = o == O_BOULDER ? RF_EV_BOULDER : RF_EV_DIAMOND_FALL;
	int below = at (g, x, y + 1);
	if (!(c & F_FALLING))
	{
		if (below == O_SPACE)
		{
			g->events |= sound;	/* (the sound as it starts to fall too) */
			move (g, x, y, x, y + 1, (uint16_t) (o | F_FALLING));
		}
		else if (rounded (g->cell[y + 1 < RF_H ? y + 1 : y][x]) && y + 1 < RF_H)
		{
			roll (g, x, y, (uint16_t) o);
		}
		return;
	}
	if (below == O_SPACE)
	{
		move (g, x, y, x, y + 1, (uint16_t) (o | F_FALLING));
		return;
	}
	if (below == O_MAGIC)
	{
		g->events |= RF_EV_DIAMOND_FALL;
		if (!g->magic_on && !g->magic_done)
		{
			g->magic_on = true;	/* (it's all on now) */
			g->magic_t = 0;
		}
		set (g, x, y, O_SPACE);
		if (g->magic_on && at (g, x, y + 2) == O_SPACE)
		{
			set (g, x, y + 2, (uint16_t) ((o == O_BOULDER ? O_DIAMOND : O_BOULDER) | F_FALLING | F_SCANNED));
		}
		return;
	}
	if (below == O_MINER || below == O_FIREFLY || below == O_BUTTERFLY)
	{
		set (g, x, y, O_SPACE);		/* (it's spent in the explosion) */
		explode (g, x, y + 1, below == O_BUTTERFLY, x, y);
		return;
	}
	g->events |= sound;
	if (!(below == O_EXPLODE || below == O_EXPLODE_DIAMOND) && rounded (g->cell[y + 1][x]) && roll (g, x, y, (uint16_t) o))
	{
		return;
	}
	set (g, x, y, (uint16_t) o);		/* (it's come to rest) */
}

/* ---- creatures -------------------------------------------------------------------------------- */

static void creature (rf_t *g, int x, int y)
{
	uint16_t c = g->cell[y][x];
	int o = rf_object (c), d = rf_dir (c);
	for (int k = 0; k < 4; k++)
	{
		int n = at (g, x + dir_dx[k], y + dir_dy[k]);
		if (n == O_MINER || n == O_AMOEBA)
		{
			explode (g, x, y, o == O_BUTTERFLY, x, y);
			return;
		}
	}
	/* a firefly turns left where it can, a butterfly right */
	int side = o == O_FIREFLY ? (d + 3) % 4 : (d + 1) % 4, other = o == O_FIREFLY ? (d + 1) % 4 : (d + 3) % 4;
	if (at (g, x + dir_dx[side], y + dir_dy[side]) == O_SPACE)
	{
		move (g, x, y, x + dir_dx[side], y + dir_dy[side], (uint16_t) (o | side << F_DIR_SHIFT));
	}
	else if (at (g, x + dir_dx[d], y + dir_dy[d]) == O_SPACE)
	{
		move (g, x, y, x + dir_dx[d], y + dir_dy[d], (uint16_t) (o | d << F_DIR_SHIFT));
	}
	else
	{
		set (g, x, y, (uint16_t) (o | other << F_DIR_SHIFT));
	}
}

static void amoeba (rf_t *g, int x, int y, bool fast)
{
	if (g->last_amoeba >= AMOEBA_TOO_BIG)
	{
		set (g, x, y, O_BOULDER);
		return;
	}
	if (!g->last_grows)
	{
		set (g, x, y, O_DIAMOND);	/* (shut in) */
		return;
	}
	g->amoeba_count++;
	for (int k = 0; k < 4 && !g->amoeba_grows; k++)
	{
		int n = at (g, x + dir_dx[k], y + dir_dy[k]);
		g->amoeba_grows = n == O_SPACE || n == O_DIRT;
	}
	if (rnd (g) % (fast ? 16 : 128) < 4)
	{
		int k = (int) (rnd (g) % 4), nx = x + dir_dx[k], ny = y + dir_dy[k], n = at (g, nx, ny);
		if (n == O_SPACE || n == O_DIRT)
		{
			bool ahead = ny > y || (ny == y && nx > x);
			set (g, nx, ny, (uint16_t) (O_AMOEBA | (ahead ? F_SCANNED : 0)));
		}
	}
}

/* ---- the miner ------------------------------------------------------------------------------- */

static void add_score (rf_t *g, int points)
{
	g->score += points;
	while (g->score >= g->next_life)
	{
		g->next_life += 500;			/* (a life every 500 points) */
		g->lives++;
		g->events |= RF_EV_EXTRA;
	}
}

static void miner (rf_t *g, int x, int y, int dx, int dy, bool fire)
{
	if (dx)
	{
		dy = 0;				/* (horizontal wins) */
	}
	g->moving = false;
	if (!dx && !dy)
	{
		return;
	}
	if (dx)
	{
		g->facing_right = dx > 0;
	}
	int tx = x + dx, ty = y + dy, t = at (g, tx, ty);
	uint16_t tc = (tx >= 0 && ty >= 0 && tx < RF_W && ty < RF_H) ? g->cell[ty][tx] : O_STEEL;
	switch (t)
	{
	case O_DIAMOND:
		g->diamonds++;
		add_score (g, g->diamonds > g->needed ? g->value_extra : g->value);
		g->events |= RF_EV_COLLECT;
		if (g->diamonds == g->needed)
		{
			g->open = true;
			g->flash = FLASH_FRAMES;
			g->events |= RF_EV_OPEN;
		}
		/* fall through */
	case O_SPACE:
	case O_DIRT:
		if (fire)
		{
			set (g, tx, ty, O_SPACE);	/* (grabbed, from where it stands) */
		}
		else
		{
			move (g, x, y, tx, ty, O_MINER);
			g->mx = tx;
			g->my = ty;
			g->moving = true;
			g->events |= RF_EV_MOVE;
		}
		break;
	case O_BOULDER:
		if (dy == 0 && !(tc & F_FALLING) && at (g, tx + dx, ty) == O_SPACE && rnd (g) % 8 == 0)	/* (1 in 8) */
		{
			move (g, tx, ty, tx + dx, ty, O_BOULDER);
			g->events |= RF_EV_PUSH;
			if (!fire)
			{
				move (g, x, y, tx, ty, O_MINER);
				g->mx = tx;
				g->my = ty;
				g->moving = true;
			}
		}
		break;
	case O_OUTBOX:
		if (!fire)
		{
			set (g, x, y, O_SPACE);
			g->mx = tx;
			g->my = ty;
			g->state = RF_EXITED;
			g->frame = 0;
		}
		break;
	}
}

/* ---- the scan --------------------------------------------------------------------------------- */

static void scan (rf_t *g, int dx, int dy, bool fire)
{
	const rf_cave_t *cave = &rf_caves[g->cave];
	bool fast = g->amoeba_t >= cave->amoeba_time * 60;
	g->amoeba_count = 0;
	g->amoeba_grows = false;
	for (int y = 0; y < RF_H; y++)
	{
		for (int x = 0; x < RF_W; x++)
		{
			uint16_t c = g->cell[y][x];
			if (c & F_SCANNED)
			{
				g->cell[y][x] = (uint16_t) (c & ~F_SCANNED);
				continue;
			}
			switch (rf_object (c))
			{
			case O_BOULDER:
			case O_DIAMOND:
				falling_thing (g, x, y);
				break;
			case O_FIREFLY:
			case O_BUTTERFLY:
				creature (g, x, y);
				break;
			case O_AMOEBA:
				amoeba (g, x, y, fast);
				break;
			case O_EXPLODE:
			case O_EXPLODE_DIAMOND:
			{
				int stage = rf_dir (c) + 1;
				if (stage > 4)
				{
					set (g, x, y, rf_object (c) == O_EXPLODE ? O_SPACE : O_DIAMOND);
				}
				else
				{
					set (g, x, y, (uint16_t) (rf_object (c) | stage << F_DIR_SHIFT));
				}
				break;
			}
			case O_MINER:
				if (g->state == RF_PLAYING)
				{
					miner (g, x, y, dx, dy, fire);
				}
				break;
			case O_INBOX:
				if (++g->inbox_scans >= INBOX_SCANS && g->state == RF_PLAYING)
				{
					set (g, x, y, O_MINER);
				}
				break;
			case O_PREOUT:
				if (g->open)
				{
					set (g, x, y, O_OUTBOX);
				}
				break;
			}
		}
	}
	g->last_amoeba = g->amoeba_count;
	g->last_grows = g->amoeba_grows;
}

/* ---- the game --------------------------------------------------------------------------------- */

void rf_start_cave (rf_t *g)
{
	const rf_cave_t *cave = &rf_caves[g->cave];
	rf_build_cave (g, g->cave);
	for (int y = 0; y < RF_H; y++)
	{
		for (int x = 0; x < RF_W; x++)
		{
			g->covered[y][x] = true;
			if (rf_object (g->cell[y][x]) == O_INBOX)
			{
				g->mx = x;
				g->my = y;
			}
		}
	}
	g->needed = cave->needed;
	g->value = cave->value;
	g->value_extra = cave->extra;
	g->diamonds = 0;
	g->time = cave->time - (g->difficulty - 1) * (cave->intermission ? 1 : 10);	/* (less time each round: this one's) */
	g->tick = 0;
	g->open = false;
	g->scan_wait = 0;
	g->inbox_scans = 0;
	g->last_amoeba = 0;
	g->last_grows = true;
	g->amoeba_t = 0;
	g->magic_on = g->magic_done = false;
	g->facing_right = false;
	g->state = RF_UNCOVER;
	g->frame = 0;
	g->events |= RF_EV_UNCOVER;
}

void rf_new_game (rf_t *g, bool demo, int hi_score)
{
	uint32_t rng = g->rng ? g->rng : 0x1984u;
	memset (g, 0, sizeof *g);
	g->rng = rng;
	g->demo = demo;
	g->hi_score = hi_score;
	g->lives = LIVES;
	g->difficulty = 1;
	g->next_life = 500;
	g->cave = 0;
	rf_start_cave (g);
}

void rf_attract (rf_t *g, int hi_score)
{
	uint32_t rng = g->rng ? g->rng : 0x1984u;
	memset (g, 0, sizeof *g);
	g->rng = rng;
	g->hi_score = hi_score;
	g->state = RF_ATTRACT;
	g->difficulty = 1;
	rf_build_cave (g, 0);
}

static bool miner_alive (const rf_t *g)
{
	for (int y = 0; y < RF_H; y++)
	{
		for (int x = 0; x < RF_W; x++)
		{
			int o = rf_object (g->cell[y][x]);
			if (o == O_MINER || o == O_INBOX)
			{
				return true;
			}
		}
	}
	return false;
}

static void next_cave (rf_t *g)
{
	g->cave++;
	if (g->cave >= RF_CAVES)
	{
		g->cave = 0;
		g->difficulty = g->difficulty < 5 ? g->difficulty + 1 : 5;	/* (round again, harder) */
	}
	rf_start_cave (g);
}

void rf_frame (rf_t *g, int dx, int dy, bool fire)
{
	g->events = 0;
	g->frame++;
	if (g->flash)
	{
		g->flash--;
	}
	g->anim++;
	switch (g->state)
	{
	case RF_ATTRACT:
	case RF_GAME_OVER:
		return;
	case RF_UNCOVER:
		for (int y = 0; y < RF_H; y++)
		{
			g->covered[y][rnd (g) % RF_W] = false;
		}
		if (g->frame >= UNCOVER_FRAMES)
		{
			memset (g->covered, 0, sizeof g->covered);
			g->state = RF_PLAYING;
			g->frame = 0;
		}
		break;
	case RF_EXITED:
		/* the time left counted into the score */
		if (g->time > 0)
		{
			g->time--;
			add_score (g, g->difficulty);
			g->events |= RF_EV_BONUS;
		}
		else if (g->frame > 30)
		{
			next_cave (g);
		}
		return;
	case RF_DEAD:
		if (g->frame >= DEAD_FRAMES || (fire && g->frame > 30))
		{
			if (!rf_caves[g->cave].intermission)
			{
				g->lives--;
			}
			if (g->lives <= 0)
			{
				g->state = RF_GAME_OVER;
				g->frame = 0;
				if (g->score > g->hi_score && !g->demo)
				{
					g->hi_score = g->score;
				}
				g->events |= RF_EV_GAME_OVER;
				return;
			}
			if (rf_caves[g->cave].intermission)
			{
				next_cave (g);		/* (an intermission isn't tried again) */
			}
			else
			{
				rf_start_cave (g);
			}
			return;
		}
		break;
	}

	/* the cave runs: a scan every few frames; its time in seconds once the miner's out */
	if (--g->scan_wait <= 0)
	{
		g->scan_wait = rf_scan_frames (g->difficulty);
		scan (g, dx, dy, fire);
		g->amoeba_t += rf_scan_frames (g->difficulty);
		if (g->state == RF_PLAYING && g->inbox_scans >= INBOX_SCANS && !miner_alive (g))
		{
			g->state = RF_DEAD;		/* (blown up) */
			g->frame = 0;
		}
	}
	if (g->magic_on && ++g->magic_t >= rf_caves[g->cave].magic_time * 60)
	{
		g->magic_on = false;
		g->magic_done = true;
	}
	if (g->state == RF_PLAYING && g->inbox_scans >= INBOX_SCANS && ++g->tick >= 60)
	{
		g->tick = 0;
		if (g->time > 0)
		{
			g->time--;
			if (g->time <= 10)
			{
				g->events |= RF_EV_TICK;
			}
		}
		if (g->time == 0)
		{
			for (int y = 0; y < RF_H; y++)	/* (out of time: the miner's gone) */
			{
				for (int x = 0; x < RF_W; x++)
				{
					if (rf_object (g->cell[y][x]) == O_MINER)
					{
						g->cell[y][x] = O_SPACE;
					}
				}
			}
			g->state = RF_DEAD;
			g->frame = 0;
			g->events |= RF_EV_DIE;
		}
	}
}

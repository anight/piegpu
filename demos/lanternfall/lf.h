/*
 * lf.h - Lanternfall's game, without its picture or its sound: Wren and her
 * lantern, Ody, the Hollow, the beacons, what the lantern shows and what Ody
 * breaks, the story's lines and the two endings, on a level of the BSP
 * engine (engine/game.h moves Wren; the level's entities say what is where:
 * lf.c). What happened in a step is in its events, for lanternfall.c to show
 * and play; bot.c plays the game by itself.
 *
 * The lantern holds an ember, 0 .. 1. Carried low it lights a few steps and
 * costs nothing. Raised, it lights a room, shows the hidden ways, kindles
 * beacons, wakes Ody, and burns the Hollow near it: and the ember drains
 * (spent, its last spark still shows and kindles, slowly, and burns nothing). A
 * flare spends a quarter at once. The Hollow's touch takes ember too; hit
 * with none left, the light goes out, and Wren is back at the last shrine or
 * beacon. Embers lie about; shrines and beacons fill the lantern.
 */
#ifndef LANTERNFALL_LF_H
#define LANTERNFALL_LF_H

#include "bsp.h"
#include "game.h"

#define LF_LEVELS		6
#define LF_MAX_BRUSHES		20		/* hidden ways, walls to break, gates */
#define LF_MAX_HOLLOW		16
#define LF_MAX_SPOTS		8		/* Ody's: where he lifts, plates he stands on */
#define LF_MAX_SHRINES		6
#define LF_MAX_STORIES		16
#define LF_MAX_BOLTS		6
#define LF_MAX_EVENTS		16
#define LF_MAX_BOT		48
#define LF_MAX_CAPTIONS		12
#define LF_MAX_CRUMBS		24

/* the lantern */
#define LF_GLOW_LOW		170.0f		/* how far it lights, carried low */
#define LF_GLOW_RAISED		430.0f		/* ... and raised */
#define LF_RAISED_SECONDS	16.0f		/* a full ember lasts this long raised */
#define LF_FLARE_COST		0.25f
#define LF_FLARE_REACH		340.0f

typedef struct
{
	game_input_t move;			/* engine/game.h: forward, side, turn, jump */
	bool raise;				/* the lantern held up */
	bool flare;				/* (as it's pressed) */
	bool ody;				/* Ody told to do what the place asks (as it's pressed) */
} lf_input_t;

/* ---- what the level has ---------------------------------------------------------------- */

enum { LF_HIDDEN, LF_BREAK, LF_GATE };		/* a brush: shown by the lantern; broken by Ody; opened by its key */
enum { LF_KEY_NONE, LF_KEY_BEACON, LF_KEY_PLATE, LF_KEY_KILL, LF_KEY_ANCHOR };

typedef struct
{
	int kind, model, solid;
	float centre[3], size[3], radius;
	int key, key_id;			/* a gate's: LF_KEY_*, and which plate or which group of the Hollow;
						   a wall's: LF_KEY_ANCHOR one of the Tide King's */
	float open;				/* 0 .. 1: shown, broken, sunk */
	bool done;
} lf_brush_t;

enum { LF_LURKER, LF_BRUTE, LF_LISTENER };
enum
{
	LF_H_ASLEEP, LF_H_STALK, LF_H_FLEE, LF_H_WINDUP, LF_H_LUNGE, LF_H_RECOVER, LF_H_STUN, LF_H_BACK, LF_H_DYING, LF_H_GONE
};

typedef struct
{
	int kind, state, group, hp;
	float home[3], p[3], v[3], yaw;
	float wake;				/* it wakes with Wren this near */
	float t;				/* seconds in its state */
	float burn;				/* 0 .. 1: a lurker in the light */
	float dir[2];				/* a lunge's */
	bool ground;
} lf_hollow_t;

enum { LF_SPOT_LIFT, LF_SPOT_PLATE };

typedef struct
{
	int kind, id;
	float p[3], top[3];			/* where; a lift's: the top of the arc Wren's thrown through */
	bool pressed;
} lf_spot_t;

enum { LF_ODY_ABSENT, LF_ODY_ASLEEP, LF_ODY_FOLLOW, LF_ODY_GO, LF_ODY_SWING, LF_ODY_KNEEL, LF_ODY_STAY };
enum { LF_DO_NONE, LF_DO_COME, LF_DO_SMASH, LF_DO_LIFT, LF_DO_STAND, LF_DO_STRIKE };

typedef struct
{
	int state;
	int task, index;			/* LF_DO_*: what he's about, and which wall, spot or Hollow */
	float p[3], v[3], yaw, goal[3];
	float t;				/* seconds in his state */
	float stride;				/* how far he's walked (his legs) */
	float swing;				/* 0 .. 1: his arm coming down */
	float stuck, wake, cool;
	bool ground;
} lf_ody_t;

typedef struct
{
	float p[3], v[3], age;
	bool live;
} lf_bolt_t;

typedef struct
{
	bool here, fighting, down, beaten;
	float home[3], p[3];
	int hp;					/* thirds of him left */
	float cast, call, stagger, burn;
} lf_king_t;

enum						/* events: what happened in the step */
{
	LF_EV_REVEAL, LF_EV_BEACON, LF_EV_SHRINE, LF_EV_EMBER, LF_EV_FLARE, LF_EV_HURT, LF_EV_OUT, LF_EV_SEA, LF_EV_SPENT,
	LF_EV_ODY_WAKE, LF_EV_ODY_STEP, LF_EV_ODY_SWING, LF_EV_ODY_SMASH, LF_EV_ODY_KNEEL, LF_EV_ODY_THROW, LF_EV_ODY_COME,
	LF_EV_HOLLOW_WAKE, LF_EV_HOLLOW_DIE, LF_EV_WINDUP, LF_EV_LUNGE, LF_EV_THUD, LF_EV_BRUTE_HURT, LF_EV_SHRIEK,
	LF_EV_GATE, LF_EV_PLATE, LF_EV_BOLT, LF_EV_KING_FALL, LF_EV_KING_HURT, LF_EV_KING_BEATEN, LF_EV_ENDING, LF_EV_CAPTION
};

typedef struct
{
	int type;
	float p[3];
} lf_event_t;

enum { LF_WHO_NONE, LF_WHO_WREN, LF_WHO_ODY, LF_WHO_ARCHIVIST, LF_WHO_KING };

typedef struct
{
	uint8_t level, id, who;
	const char *text;
} lf_line_t;

/* the lines' ids that the game itself says (the others: the level's lf_story triggers') */
enum
{
	LF_LINE_START = 0, LF_LINE_BEACON = 100, LF_LINE_ODY_WAKE, LF_LINE_HOLLOW, LF_LINE_OUT, LF_LINE_BRUTE, LF_LINE_CAUGHT,
	LF_LINE_KILL, LF_LINE_FIGHT = 110, LF_LINE_STAGGER, LF_LINE_BEATEN, LF_LINE_RELIT = 200, LF_LINE_DARK
};

typedef struct
{
	float p[3], radius;
	int line;
	bool told;
} lf_story_t;

typedef struct
{
	float p[3];
	char what[12];				/* "", raise, ody, fight, sneak, wait */
} lf_step_t;

/* ---- the game --------------------------------------------------------------------------- */

typedef struct
{
	const bsp_t *bsp;
	int level;
	game_t g;				/* Wren, and the level's doors and lifts */

	/* the lantern */
	float ember, raised;			/* 0 .. 1 each */
	float flare_cool, flash;		/* the flare: till the next; its light, fading */
	float hurt;				/* seconds she can't be hurt again */
	float out;				/* the light gone out: seconds till she's back (0: not) */
	float sea;				/* in the sea: likewise */
	float safe[3], check[3], check_yaw;	/* the last firm ground; the last shrine or beacon */
	bool spent_said;

	lf_brush_t brush[LF_MAX_BRUSHES];
	int n_brushes;
	lf_hollow_t hollow[LF_MAX_HOLLOW];
	int n_hollow;
	lf_spot_t spot[LF_MAX_SPOTS];
	int n_spots;
	float shrine[LF_MAX_SHRINES][3];
	int n_shrines, at_shrine;		/* (the shrine she's at: -1 none) */
	lf_story_t story[LF_MAX_STORIES];
	int n_stories;
	lf_bolt_t bolt[LF_MAX_BOLTS];
	lf_ody_t ody;
	lf_king_t king;
	float crumb[LF_MAX_CRUMBS][3];		/* where Wren walked: Ody's way after her */
	int crumbs;

	bool has_beacon, beacon_lit;
	float beacon[3], kindle;
	bool has_exit;
	float exit[3], exit_radius;
	float choice[2][3], choosing[2];	/* the peak: the great lantern, the tide pool; 0 .. 1 */
	bool has_choice;
	float archivist[3], archivist_yaw;
	bool has_archivist;

	int ody_can;				/* LF_DO_*: what Y would have Ody do now */
	bool done;				/* the level's left: on to the next */
	int ending;				/* 0, or 1 the lantern relit, 2 the dark kept */
	bool said_hollow, said_brute, said_out;
	int deaths;

	lf_event_t event[LF_MAX_EVENTS];
	int n_events;

	/* the story's lines: waiting, and the one shown */
	const lf_line_t *caption[LF_MAX_CAPTIONS];
	int n_captions;
	float caption_t;			/* seconds the first has been shown */

	/* the way through, for bot.c */
	lf_step_t step[LF_MAX_BOT];
	int n_steps;
	float time;
} lf_t;

/* a level begun: its entities read (false: no room for them), Wren at its start with `ember` */
bool lf_load (lf_t *s, const bsp_t *bsp, int level, float ember);
/* a step of dt seconds */
void lf_update (lf_t *s, const lf_input_t *in, float dt);

/* where the lantern is, and how far it lights now */
void lf_lantern (const lf_t *s, float p[3]);
float lf_glow (const lf_t *s);
/* the line shown now (NULL: none), and how long each is shown */
const lf_line_t *lf_caption (const lf_t *s);
float lf_caption_seconds (const lf_line_t *l);
const char *lf_level_name (int level);
/* a Hollow's size (its middle over the floor, how near it touches) */
float lf_hollow_reach (const lf_hollow_t *h);

/* the story (text.c) */
extern const lf_line_t lf_lines[];
extern const int lf_n_lines;

/* ---- the game played by itself (bot.c) ---------------------------------------------------------- */

typedef struct
{
	game_pilot_t pilot;
	float points[LF_MAX_BOT][3];
	int at;					/* the step it's doing what's asked at (-1: walking) */
	float t, since;
	int check_step;				/* where it was at the last shrine or beacon */
	float check[3], cool;
	bool was_out, pressed;
	int deaths;
} lf_bot_t;

void lf_bot_init (lf_bot_t *b, const lf_t *s);
void lf_bot (lf_bot_t *b, const lf_t *s, lf_input_t *in, float dt);

#endif

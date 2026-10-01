/*
 * pad.h - a stick and buttons, where the host's board has them: the Pico's
 * analog stick and its game controller (hosts/pico/input: both optional,
 * found at the start). A host without them links pad_none.c.
 */
#ifndef ENGINE_PAD_H
#define ENGINE_PAD_H

#include <stdbool.h>

enum
{
	PAD_A = 1, PAD_B = 2, PAD_X = 4, PAD_Y = 8, PAD_SELECT = 16, PAD_START = 32,	/* the controller's */
	PAD_STICK = 64									/* the stick pressed */
};

typedef struct
{
	float x, y;			/* -1 (left, back) .. 1 (right, forward); 0 at rest */
	unsigned buttons;		/* PAD_*: held now */
} pad_t;

/* look for them (the stick at rest); false: there's none */
bool pad_init (void);
/* how they are now (the stick and the controller's as one: the further of
   the two each way); false, and all at rest, without them */
bool pad_read (pad_t *p);

#endif

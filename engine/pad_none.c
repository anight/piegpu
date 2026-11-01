/*
 * pad_none.c - pad.h for a host with no stick and no game controller
 */
#include "pad.h"
#include <string.h>

bool pad_init (void)
{
	return false;
}

bool pad_read (pad_t *p)
{
	memset (p, 0, sizeof *p);
	return false;
}

bool pad_read_player (int player, pad_t *p)
{
	(void) player;
	return pad_read (p);
}

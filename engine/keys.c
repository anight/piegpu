/*
 * keys.c - see keys.h
 */
#include "keys.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pico/stdlib.h"
#ifdef ENGINE_RAW_TERMINAL
#include <signal.h>
#include <termios.h>
#include <unistd.h>

static struct termios saved;
static struct sigaction previous[3];
static const int signals[3] = {SIGINT, SIGTERM, SIGHUP};

static void restore (void)		{ tcsetattr (0, TCSANOW, &saved); }

static void on_signal (int sig)
{
	restore ();
	for (int i = 0; i < 3; i++)
	{
		if (signals[i] == sig && previous[i].sa_handler != SIG_DFL && previous[i].sa_handler != SIG_IGN)
		{
			previous[i].sa_handler (sig);	/* (the transport's: the session's end) */
		}
	}
	_exit (128 + sig);
}

static void raw_terminal (void)
{
	if (!isatty (0) || tcgetattr (0, &saved) != 0)
	{
		return;
	}
	struct termios t = saved;
	t.c_lflag &= ~(ICANON | ECHO);
	t.c_cc[VMIN] = 0;
	t.c_cc[VTIME] = 0;
	tcsetattr (0, TCSANOW, &t);
	atexit (restore);
	struct sigaction sa;
	memset (&sa, 0, sizeof sa);
	sa.sa_handler = on_signal;
	for (int i = 0; i < 3; i++)
	{
		sigaction (signals[i], &sa, &previous[i]);
	}
}
#endif

void keys_init (keys_t *k)
{
	memset (k, 0, sizeof *k);
	k->idle = 1e9f;
#ifdef ENGINE_RAW_TERMINAL
	raw_terminal ();
#endif
}

/* the keys come in: each held again; true if any */
static bool read_keys (keys_t *k)
{
	static int escape;			/* ESC [ x: the arrows */
	bool any = false;
	int c;
	while ((c = getchar_timeout_us (0)) != PICO_ERROR_TIMEOUT)
	{
		int key = -1;
		if (escape == 1)
		{
			escape = c == '[' ? 2 : 0;
			continue;
		}
		if (escape == 2)
		{
			key = c == 'A' ? KEYS_FORWARD : c == 'B' ? KEYS_BACK : c == 'C' ? KEYS_TURN_RIGHT : c == 'D' ? KEYS_TURN_LEFT : -1;
			escape = 0;
		}
		else if (c == 27)
		{
			escape = 1;
			continue;
		}
		else
		{
			switch (c)
			{
			case 'w': case 'W': key = KEYS_FORWARD; break;
			case 's': case 'S': key = KEYS_BACK; break;
			case 'a': case 'A': key = KEYS_LEFT; break;
			case 'd': case 'D': key = KEYS_RIGHT; break;
			case 'q': case 'Q': key = KEYS_TURN_LEFT; break;
			case 'e': case 'E': key = KEYS_TURN_RIGHT; break;
			case ' ': key = KEYS_JUMP; break;
			}
		}
		if (key >= 0)
		{
			k->held[key] = KEYS_HOLD_S;
			any = true;
		}
	}
	return any;
}

void keys_input (keys_t *k, const game_t *g, game_input_t *in, float dt)
{
	k->idle = read_keys (k) ? 0.0f : k->idle + dt;
	memset (in, 0, sizeof *in);
	in->forward = (k->held[KEYS_FORWARD] > 0) - (k->held[KEYS_BACK] > 0);
	in->side = (k->held[KEYS_RIGHT] > 0) - (k->held[KEYS_LEFT] > 0);
	in->turn = KEYS_TURN * ((k->held[KEYS_TURN_LEFT] > 0) - (k->held[KEYS_TURN_RIGHT] > 0));
	in->look = -g->pitch * 2.0f;
	in->jump = k->held[KEYS_JUMP] > 0;
	for (int i = 0; i < KEYS_N; i++)
	{
		k->held[i] = k->held[i] > dt ? k->held[i] - dt : 0.0f;
	}
}

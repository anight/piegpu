/*
 * sim.c - Lanternfall without a GPU (a PC): the game played by itself
 * (bot.c) through its levels, as fast as the PC goes, and what happened on
 * the console. Is every level's way open from its start to its end?
 *
 *   lanternfall_sim LEVELS_DIR [FIRST [LAST [ENDING]]]	(levels 0 .. 5; ENDING 1 or 2: the choice at the peak)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lf.h"

static const char *const files[LF_LEVELS] = {"docks", "library", "forest", "market", "stair", "peak"};
static const char *const events[] =
{
	"reveal", "beacon", "shrine", "ember", "flare", "hurt", "out", "sea", "spent", "ody-wake", "ody-step", "ody-swing", "ody-smash",
	"ody-kneel", "ody-throw", "ody-come", "hollow-wake", "hollow-die", "windup", "lunge", "thud", "brute-hurt", "shriek", "gate",
	"plate", "bolt", "king-fall", "king-hurt", "king-beaten", "ending", "caption"
};

int main (int argc, char **argv)
{
	const char *dir = argc > 1 ? argv[1] : ".";
	int first = argc > 2 ? atoi (argv[2]) : 0, last = argc > 3 ? atoi (argv[3]) : LF_LEVELS - 1;
	bool verbose = getenv ("LF_VERBOSE") != NULL;
	float ember = 0.6f;
	static lf_t s;
	static lf_bot_t bot;
	for (int level = first; level <= last; level++)
	{
		char path[512];
		snprintf (path, sizeof path, "%s/%s.bsp", dir, files[level]);
		FILE *f = fopen (path, "rb");
		if (!f)
		{
			printf ("%s: not there\n", path);
			continue;
		}
		fseek (f, 0, SEEK_END);
		long size = ftell (f);
		fseek (f, 0, SEEK_SET);
		void *data = malloc ((size_t) size);
		static bsp_t bsp;
		if (fread (data, 1, (size_t) size, f) != (size_t) size || !bsp_load (&bsp, data, (size_t) size) || !lf_load (&s, &bsp, level, ember))
		{
			printf ("%s: not loaded\n", path);
			return 1;
		}
		fclose (f);
		lf_bot_init (&bot, &s);
		int counts[sizeof events / sizeof events[0]] = {0}, lines = 0;
		float t = 0.0f, low = 1.0f;
		const lf_line_t *shown = NULL;
		while (!s.done && !(s.ending && !s.n_captions) && t < 900.0f)
		{
			lf_input_t in;
			lf_bot (&bot, &s, &in, 1.0f / 60);
			if (s.has_choice && argc > 4 && atoi (argv[4]) == 2 && !s.ending)	/* (the other ending: not the lantern) */
			{
				s.choosing[0] = 0.0f;
			}
			lf_update (&s, &in, 1.0f / 60);
			t += 1.0f / 60;
			low = s.ember < low ? s.ember : low;
			if (getenv ("LF_TRACE") && (int) (t * 4) != (int) ((t - 1.0f / 60) * 4))
			{
				printf ("  %6.2f s  Wren %.0f %.0f %.0f v %.0f %.0f %.0f ground %d, step %d at %d, raise %d ember %.2f, Ody %d at %.0f %.0f %.0f\n", t,
					s.g.origin[0], s.g.origin[1], s.g.origin[2], s.g.velocity[0], s.g.velocity[1], s.g.velocity[2], s.g.on_ground,
					bot.pilot.next, bot.at, in.raise, s.ember, s.ody.state, s.ody.p[0], s.ody.p[1], s.ody.p[2]);
			}
			for (int i = 0; i < s.n_events; i++)
			{
				counts[s.event[i].type]++;
				if (verbose && s.event[i].type != LF_EV_ODY_STEP && s.event[i].type != LF_EV_CAPTION)
				{
					printf ("  %6.1f s  %-12s at %.0f %.0f %.0f (Wren %.0f %.0f %.0f, ember %.2f, step %d)\n", t,
						events[s.event[i].type], s.event[i].p[0], s.event[i].p[1], s.event[i].p[2], s.g.origin[0],
						s.g.origin[1], s.g.origin[2], s.ember, bot.pilot.next);
				}
			}
			if (lf_caption (&s) != shown)
			{
				shown = lf_caption (&s);
				lines += shown != NULL;
				if (verbose && shown)
				{
					printf ("  %6.1f s  \"%s\"\n", t, shown->text);
				}
			}
		}
		printf ("%d %-20s %s in %5.1f s: ember %.2f (lowest %.2f), out %d, in the sea %d, hurt %d, %d lines; ", level,
			lf_level_name (level), s.done ? "left" : s.ending == 1 ? "ENDING: the lantern relit" : s.ending == 2 ? "ENDING: the dark kept"
			: "NOT DONE", t, s.ember, low, counts[LF_EV_OUT], counts[LF_EV_SEA], counts[LF_EV_HURT], lines);
		for (unsigned i = 0; i < sizeof events / sizeof events[0]; i++)
		{
			if (counts[i] && i != LF_EV_ODY_STEP && i != LF_EV_CAPTION && i != LF_EV_OUT && i != LF_EV_SEA && i != LF_EV_HURT)
			{
				printf ("%s %d, ", events[i], counts[i]);
			}
		}
		printf ("%s\n", s.done || s.ending ? "" : "");
		if (!s.done && !s.ending)
		{
			printf ("  stopped at step %d of %d (doing %d), Wren at %.0f %.0f %.0f, Ody state %d at %.0f %.0f %.0f\n", bot.pilot.next,
				s.n_steps, bot.at, s.g.origin[0], s.g.origin[1], s.g.origin[2], s.ody.state, s.ody.p[0], s.ody.p[1], s.ody.p[2]);
			return 1;
		}
		ember = s.ember;
		free (data);
	}
	return 0;
}

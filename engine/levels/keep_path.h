/*
 * keep_path.h - the autopilot's way round engine/levels/keep.bsp (the
 * player's origins, 24 over the floor; engine/game.h): by every gem, across
 * the bridge, through the portcullis, up the lift, over the sky bridge, along
 * the walls and down the stairs
 */
static const float keep_path[][3] =
{
	{384, 384, 24},			/* a gem in the courtyard */
	{1024, 480, 24},
	{1024, 672, 24},		/* a gem on the bridge */
	{1024, 800, 24},		/* the portcullis */
	{1024, 900, 24},
	{960, 1000, 24},		/* a gem inside */
	{1136, 1136, 40},		/* onto the lift ... */
	{1136, 1136, 232},		/* ... up */
	{920, 940, 232},		/* a gem upstairs */
	{1024, 1150, 232},
	{1024, 1568, 288},		/* a gem on the sky bridge */
	{1024, 1984, 344},		/* the north walkway */
	{1984, 1984, 344},		/* a gem in its corner */
	{1984, 1760, 344},
	{1856, 1760, 344},		/* the stairs' landing, then down */
	{1856, 1468, 184},		/* a gem on them */
	{1856, 1200, 24},
	{1760, 960, 24},		/* a gem in the courtyard */
	{1760, 640, 24},
	{1600, 420, 24},		/* a gem by the well */
	{1024, 256, 24},		/* the start */
};
#define KEEP_PATH	(int) (sizeof keep_path / sizeof keep_path[0])

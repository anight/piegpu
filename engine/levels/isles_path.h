/*
 * isles_path.h - the autopilot's way round engine/levels/isles.bsp (the
 * player's origins, 24 over the floor; engine/game.h): by every coin, on the
 * platform across, up by the jump pad, down the bridge, home by the portal
 */
static const float isles_path[][3] =
{
	{64, 0, 24},			/* a coin */
	{320, 0, 24},			/* the platform's stop: waits at the edge for it */
	{800, 0, 24},			/* the next island: waits on the platform till there */
	{1000, -100, 24},		/* a coin */
	{896, 128, 28},			/* the jump pad: thrown (a coin in the arc) */
	{1000, 800, 280},		/* coins on the high island */
	{760, 896, 280},		/* the bridge, down */
	{240, 896, 152},		/* (a coin on it) */
	{0, 896, 152},			/* a coin in the ruins */
	{-170, 896, 152},		/* the portal: home */
};
#define ISLES_PATH	(int) (sizeof isles_path / sizeof isles_path[0])

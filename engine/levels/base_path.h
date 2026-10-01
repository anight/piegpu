/* base_path.h - a walk round the base level (engine/tools/make_base.py): the
   player's origins (24 over the floor), for game_pilot. From the start hall
   through the door to the atrium, up the stairs to the mezzanine, along the
   high corridor to the brick hall, down its ramp and back to the start */
static const float base_path[][3] =
{
	{560, 256, 24}, {760, 256, 24}, {1200, 256, 24}, {1400, 256, 24}, {1750, 300, 24},
	{1760, 560, 24}, {1896, 704, 24}, {1548, 704, 280}, {1400, 640, 280}, {1250, 640, 280},
	{800, 640, 280}, {740, 640, 280}, {300, 640, 24}, {300, 850, 24}, {180, 640, 24},
	{180, 520, 24}, {180, 400, 24}, {300, 256, 24},
};
#define BASE_PATH	(int) (sizeof base_path / sizeof base_path[0])

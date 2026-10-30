/*
 * render_programs.h - Lanternfall's shaders for the BSP engine's renderer, in
 * place of the engine's own (engine/render.c, RENDER_OWN_PROGRAMS): the level
 * in the lantern's light, the black sea, the overcast sky (shaders/)
 */
#include "lf_world_program.h"
#include "lf_liquid_program.h"
#include "lf_skydome_program.h"
#define RENDER_WORLD	lf_world_info
#define RENDER_LIQUID	lf_liquid_info
#define RENDER_SKY	lf_skydome_info

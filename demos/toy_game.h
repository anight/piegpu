/*
 * toy_game.h - the game side of a Shadertoy-style game (toy.c with a game
 * source, CMakeLists.txt toy_demo(... GAME)): the Pico keeps the game state
 * and plays it (as a Shadertoy game's state buffer would), the fragment
 * shader draws the whole screen from uniforms (and textures) it is given.
 */
#ifndef TOY_GAME_H
#define TOY_GAME_H

#include "gles/pgl.h"

/* once: the program is linked and in use; the screen is width x height pixels */
void game_init (GLuint prog, float width, float height);
/* the screen has a new shape: the game goes on in a width x height world */
void game_resize (float width, float height);
/* every frame, before the draw (the program in use): advance by dt seconds,
   set the uniforms */
void game_frame (float t, float dt);

#endif

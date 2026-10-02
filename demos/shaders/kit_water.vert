// kit_water: the sea of the Jet scenes' island (demos/jet/scenes/tropical-island.cpp)
attribute vec4 a_pos;

uniform mat4 u_mvp;

void main ()
{
	gl_Position = u_mvp * vec4 (a_pos.xyz, 1.0);
}

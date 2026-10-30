// world: Lanternfall's level (the BSP engine's faces, engine/render.c: this
// in place of the engine's own "world"); the place of each fragment goes on,
// for the lantern's light and the fog
attribute vec3 a_pos;
attribute vec2 a_uv;
attribute vec2 a_luv;

uniform mat4 u_vp;
uniform vec3 u_offset;

varying vec2 v_uv;
varying vec2 v_luv;
varying vec3 v_pos;

void main ()
{
	vec3 p = a_pos + u_offset;
	v_uv = a_uv;
	v_luv = a_luv;
	v_pos = p;
	gl_Position = u_vp * vec4 (p, 1.0);
}

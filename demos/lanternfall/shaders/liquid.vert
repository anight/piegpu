// liquid: Lanternfall's sea and its flooded floors (in place of the engine's
// "liquid")
attribute vec3 a_pos;
attribute vec2 a_uv;

uniform mat4 u_vp;
uniform vec3 u_offset;

varying vec2 v_uv;
varying vec3 v_pos;

void main ()
{
	vec3 p = a_pos + u_offset;
	v_uv = a_uv;
	v_pos = p;
	gl_Position = u_vp * vec4 (p, 1.0);
}

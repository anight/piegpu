// world: a BSP level's faces (engine/render.c): the texture's and the
// lightmap's coordinates as the host worked them out; brush models moved by
// u_offset
attribute vec3 a_pos;
attribute vec2 a_uv;
attribute vec2 a_luv;

uniform mat4 u_vp;
uniform vec3 u_offset;

varying vec2 v_uv;
varying vec2 v_luv;

void main ()
{
	v_uv = a_uv;
	v_luv = a_luv;
	gl_Position = u_vp * vec4 (a_pos + u_offset, 1.0);
}

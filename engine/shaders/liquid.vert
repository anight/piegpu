// liquid: a liquid's faces (engine/render.c: textures *NAME), unlit; brush
// models moved by u_offset
attribute vec3 a_pos;
attribute vec2 a_uv;

uniform mat4 u_vp;
uniform vec3 u_offset;

varying vec2 v_uv;

void main ()
{
	v_uv = a_uv;
	gl_Position = u_vp * vec4 (a_pos + u_offset, 1.0);
}

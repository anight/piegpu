// GL API test (gles/pgl.c): uniforms of every kind, looked up by name
attribute vec2 a_pos;
attribute vec2 a_uv;
attribute vec4 a_color;
uniform mat4 u_mvp;
uniform vec2 u_offset[2];
uniform int u_which;
varying vec2 v_uv;
varying vec4 v_color;

void main ()
{
	vec2 o = u_which == 1 ? u_offset[1] : u_offset[0];
	gl_Position = u_mvp * vec4 (a_pos + o, 0.0, 1.0);
	v_uv = a_uv;
	v_color = a_color;
}

// video.vert - a video texture on a quad (demos/video.c): the texture's row 0
// is the picture's top
attribute vec2 a_pos;
uniform vec2 u_scale;
varying vec2 v_uv;

void main ()
{
	v_uv = vec2 (a_pos.x * 0.5 + 0.5, 0.5 - a_pos.y * 0.5);
	gl_Position = vec4 (a_pos * u_scale, 0.0, 1.0);
}

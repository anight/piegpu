// hud: text and bars in pixels (y down), coloured per vertex
attribute vec2 a_pos;
attribute vec2 a_uv;
attribute vec4 a_color;

uniform vec2 u_scale;		// (2 / width, -2 / height)

varying vec2 v_uv;
varying vec4 v_color;

void main ()
{
	v_uv = a_uv;
	v_color = a_color;
	gl_Position = vec4 (a_pos * u_scale + vec2 (-1.0, 1.0), 0.0, 1.0);
}

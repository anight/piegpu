// brush: a round dab, a unit square placed by u_rect (x, y, width, height in
// clip space)
attribute vec2 a_pos;
uniform vec4 u_rect;
varying vec2 v_pos;

void main ()
{
	v_pos = a_pos * 2.0 - 1.0;
	gl_Position = vec4 (u_rect.xy + a_pos * u_rect.zw, 0.0, 1.0);
}

// a unit-square shape placed by u_rect (x, y, width, height in clip space)
attribute vec2 a_pos;
uniform vec4 u_rect;

void main ()
{
	gl_Position = vec4 (u_rect.xy + a_pos * u_rect.zw, 0.0, 1.0);
}

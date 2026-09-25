// built-in variable test: a unit-square shape placed by u_rect (clip space)
attribute vec2 a_pos;
uniform vec4 u_rect;
uniform float u_point_size;

void main ()
{
	gl_Position = vec4 (u_rect.xy + a_pos * u_rect.zw, 0.0, 1.0);
	gl_PointSize = u_point_size;
}

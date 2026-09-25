// glow: round, additive points (particles, stars); alpha = brightness and size
attribute vec3 a_pos;
attribute vec4 a_color;

uniform mat4 u_vp;
uniform float u_size;		// pixels at distance 1

varying vec4 v_color;

void main ()
{
	gl_Position = u_vp * vec4 (a_pos, 1.0);
	gl_PointSize = u_size * (0.35 + 0.65 * a_color.a) / gl_Position.w;
	v_color = a_color;
}

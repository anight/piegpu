// rising sparks: point sprites sized by distance
attribute vec3 a_pos;
attribute vec4 a_color;

uniform mat4 u_mvp;
uniform float u_time;
uniform float u_size;

varying vec4 v_color;

void main ()
{
	vec3 p = a_pos;
	p.y = mod (p.y + u_time * 0.6, 3.0) - 1.5;
	p.x += 0.15 * sin (u_time * 2.0 + a_pos.z * 5.0);
	gl_Position = u_mvp * vec4 (p, 1.0);
	gl_PointSize = u_size / gl_Position.w;
	v_color = a_color * (1.0 - (p.y + 1.5) / 3.0);
}

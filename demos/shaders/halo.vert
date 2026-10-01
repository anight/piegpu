// halo: a glow round a gem, a square facing the eye (its corners -1 .. 1
// along the view's right and up, as big as u_size), added to what's there
attribute vec2 a_corner;

uniform mat4 u_vp;
uniform vec3 u_center;
uniform vec3 u_right;			// the view's, unit
uniform vec3 u_up;
uniform float u_size;

varying vec2 v_corner;

void main ()
{
	v_corner = a_corner;
	vec3 p = u_center + (u_right * a_corner.x + u_up * a_corner.y) * u_size;
	gl_Position = u_vp * vec4 (p, 1.0);
}

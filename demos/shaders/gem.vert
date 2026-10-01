// gem: keep's gems, faceted (flat normals), lit by the low sun, with a
// glint towards the eye and a glow of their own
attribute vec3 a_pos;
attribute vec3 a_normal;

uniform mat4 u_vp;
uniform mat4 u_model;
uniform vec3 u_sun;			// towards the sun
uniform vec3 u_eye;
uniform vec3 u_color;

varying vec3 v_color;

void main ()
{
	vec4 p = u_model * vec4 (a_pos, 1.0);
	vec3 n = normalize ((u_model * vec4 (a_normal, 0.0)).xyz);
	vec3 v = normalize (u_eye - p.xyz);
	float diffuse = max (dot (n, u_sun), 0.0);
	float glint = pow (max (dot (reflect (-u_sun, n), v), 0.0), 12.0);
	float rim = 1.0 - abs (dot (n, v));
	v_color = u_color * (0.45 + 0.8 * diffuse + 0.5 * rim) + vec3 (glint * 0.9);
	gl_Position = u_vp * p;
}

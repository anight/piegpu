// craft: antigrav's racers, flat-shaded faces lit by the sun, fogged
attribute vec3 a_pos;
attribute vec3 a_normal;
attribute vec2 a_uv;

uniform mat4 u_vp;
uniform mat4 u_model;
uniform vec3 u_sun_dir;		// towards the sun
uniform vec3 u_eye;
uniform float u_fog;

varying vec2 v_uv;
varying float v_light;
varying float v_fog;

void main ()
{
	vec4 p = u_model * vec4 (a_pos, 1.0);
	vec3 n = normalize ((u_model * vec4 (a_normal, 0.0)).xyz);
	v_light = 0.42 + 0.75 * max (dot (n, u_sun_dir), 0.0) + 0.12 * max (n.y, 0.0);
	v_uv = a_uv;
	v_fog = 1.0 - exp (-length (p.xyz - u_eye) * u_fog);
	gl_Position = u_vp * p;
}

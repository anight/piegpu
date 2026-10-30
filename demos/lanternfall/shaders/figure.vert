// figure: Lanternfall's people and things: flat faces, each vertex with its
// face's normal and colour, lit here: by the light where it stands (u_ambient:
// the level's, under it), by the lantern and the nearest fire, and a sheen at
// its edges (u_rim: the Hollow's). A colour's alpha 0: it glows, unlit (an
// eye, the lantern's ember). u_wobble: the Hollow's skin moving.
attribute vec3 a_pos;
attribute vec3 a_normal;
attribute vec4 a_color;

uniform mat4 u_vp;
uniform mat4 u_model;
uniform vec3 u_eye;
uniform vec3 u_ambient;
uniform vec4 u_lantern;
uniform vec3 u_gold;
uniform vec4 u_fire;
uniform vec3 u_fire_gold;
uniform vec3 u_rim;
uniform vec3 u_tint;
uniform mediump vec4 u_fog;
uniform float u_time;
uniform float u_wobble;

varying vec3 v_color;
varying float v_fog;

void main ()
{
	vec3 lp = a_pos + a_normal * (u_wobble * sin (u_time * 5.0 + a_pos.z * 0.21 + a_pos.x * 0.17 + a_pos.y * 0.13));
	vec4 p = u_model * vec4 (lp, 1.0);
	vec3 n = normalize ((u_model * vec4 (a_normal, 0.0)).xyz);
	vec3 light = u_ambient * (0.65 + 0.35 * n.z);
	vec3 d = u_lantern.xyz - p.xyz;
	float dd = dot (d, d);
	float a = max (1.0 - dd * u_lantern.w, 0.0);
	light += u_gold * (a * a * (0.35 + 0.65 * max (dot (n, d) * inversesqrt (dd + 1.0), 0.0)));
	d = u_fire.xyz - p.xyz;
	dd = dot (d, d);
	a = max (1.0 - dd * u_fire.w, 0.0);
	light += u_fire_gold * (a * a * (0.35 + 0.65 * max (dot (n, d) * inversesqrt (dd + 1.0), 0.0)));
	vec3 v = u_eye - p.xyz;
	float rim = 1.0 - abs (dot (n, v) * inversesqrt (dot (v, v)));
	light += u_rim * (rim * rim);
	v_color = mix (a_color.rgb * 1.25, a_color.rgb * light, a_color.a) * u_tint;
	float f = min (length (p.xyz - u_eye) * u_fog.w, 1.0);
	v_fog = f * f;
	gl_Position = u_vp * p;
}

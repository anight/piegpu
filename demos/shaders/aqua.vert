// aquarium: what stands still under water (the sand, the rocks, the glass,
// the surface from below) and what sways (the plants: a_sway says how much of
// the current a vertex takes). Lit from above through the water: the light of
// the sky, the sun's, the caustics on what faces up, all fading into the
// water's colour with the distance
attribute vec3 a_pos;
attribute vec3 a_normal;
attribute vec2 a_uv;
attribute float a_sway;

uniform mat4 u_vp;
uniform vec3 u_eye;
uniform vec4 u_time;		// seconds; the caustics' scroll (x, z); how much of them the surface shows (it glows)
uniform vec4 u_water;		// the fog: where it starts, its density; the tank's height; -
uniform vec3 u_deep;		// the water's colour at the floor ...
uniform vec3 u_shallow;		// ... and under the surface

varying vec2 v_uv;
varying vec4 v_cuv;
varying vec4 v_light;		// the light; the caustics' share; the shine; how clear the water is to here
varying vec3 v_fogc;

void main ()
{
	vec3 p = a_pos;
	float wave = u_time.x * 1.1 + p.x * 0.6 + p.z * 0.9;
	p.x += a_sway * (0.30 * sin (wave) + 0.10 * sin (wave * 2.3 + 1.0));
	p.z += a_sway * 0.18 * cos (wave * 0.8);

	vec3 n = a_normal;
	const vec3 sun = vec3 (0.24, 0.94, 0.24);
	float up = max (n.y, 0.0);
	float depth = clamp (p.y / u_water.z, 0.0, 1.0);
	v_light.x = 0.40 + 0.45 * max (dot (n, sun), 0.0) + 0.15 * up + u_time.w * 0.5;
	v_light.y = up * (0.55 + 0.45 * depth) + u_time.w;
	v_light.z = 0.0;
	float d = length (p - u_eye) - u_water.x;
	v_light.w = exp (-max (d, 0.0) * u_water.y);
	v_fogc = mix (u_deep, u_shallow, depth);
	v_uv = a_uv;
	v_cuv = vec4 (p.xz * 0.11 + u_time.yz, p.xz * 0.16 - u_time.yz * 1.4 + vec2 (0.37, 0.61));
	gl_Position = u_vp * vec4 (p, 1.0);
}

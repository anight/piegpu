// aquarium: a fish. Its mesh lies along x (the nose at 0.5, the tail's tip at
// -0.5); a wave runs down its body, wider towards the tail: that's how it
// swims. Lit as the rest (aqua.vert), with the shine of its scales
attribute vec3 a_pos;
attribute vec3 a_normal;
attribute vec2 a_uv;

uniform mat4 u_vp;
uniform mat4 u_model;
uniform vec3 u_eye;
uniform vec4 u_time;		// seconds; the caustics' scroll (x, z); -
uniform vec4 u_water;		// the fog: where it starts, its density; the tank's height; -
uniform vec3 u_deep;
uniform vec3 u_shallow;
uniform vec4 u_swim;		// the wave's phase, how wide it is; -; -

varying vec2 v_uv;
varying vec4 v_cuv;
varying vec4 v_light;
varying vec3 v_fogc;

void main ()
{
	float along = 0.5 - a_pos.x;			// 0 at the nose, 1 at the tail's tip
	float bend = along * along;
	float phase = u_swim.x - along * 5.5;
	vec3 local = a_pos;
	local.z += u_swim.y * (sin (phase) * bend + 0.12 * sin (u_swim.x) * (1.0 - along));
	vec3 ln = a_normal;
	ln.x += u_swim.y * cos (phase) * bend * 4.0 * a_normal.z;	// (the sides lean with the wave)

	vec4 p = u_model * vec4 (local, 1.0);
	vec3 n = normalize ((u_model * vec4 (ln, 0.0)).xyz);
	const vec3 sun = vec3 (0.24, 0.94, 0.24);
	float up = max (n.y, 0.0);
	float depth = clamp (p.y / u_water.z, 0.0, 1.0);
	vec3 view = normalize (u_eye - p.xyz);
	float side = abs (dot (n, view));		// (a fin's both sides are lit)
	v_light.x = 0.42 + 0.40 * max (dot (n, sun), 0.0) + 0.13 * up + 0.10 * side;
	v_light.y = up * (0.50 + 0.40 * depth);
	float shine = max (dot (n, normalize (sun + view)), 0.0);
	shine *= shine;
	shine *= shine;
	v_light.z = 0.28 * shine * shine;
	float d = length (p.xyz - u_eye) - u_water.x;
	v_light.w = exp (-max (d, 0.0) * u_water.y);
	v_fogc = mix (u_deep, u_shallow, depth);
	v_uv = a_uv;
	v_cuv = vec4 (p.xz * 0.11 + u_time.yz, p.xz * 0.16 - u_time.yz * 1.4 + vec2 (0.37, 0.61));
	gl_Position = u_vp * p;
}

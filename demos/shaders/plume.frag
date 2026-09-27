// plume: glowing gas, brightest where the view passes through the most of it
// (the middle of the cone, the nozzle seen from behind); the outer flame
// orange to dark red, the core white-hot to blue with shock diamonds.
// Blended ONE, ONE, both sides of the cone.
precision mediump float;

uniform highp float u_time;	// also in plume.vert: the same precision
uniform float u_core;		// 0: the outer flame, 1: the core

varying float v_u;
varying float v_facing;
varying float v_cap;

void main ()
{
	float u = v_u;
	float flicker = 0.85 + 0.15 * sin (u * 19.0 - u_time * 43.0) * sin (u * 7.0 + u_time * 29.0);
	float body = v_cap >= 0.0 ? 1.3 * (1.0 - v_cap * v_cap) : pow (v_facing, 1.5);
	float fade = pow (1.0 - u, 1.2);

	vec3 outer = mix (mix (vec3 (1.0, 0.72, 0.32), vec3 (1.0, 0.38, 0.08), smoothstep (0.0, 0.45, u)),
			  vec3 (0.45, 0.08, 0.03), smoothstep (0.45, 1.0, u));
	vec3 core = mix (vec3 (1.0, 0.97, 0.88), vec3 (0.55, 0.7, 1.0), smoothstep (0.15, 1.0, u));
	float diamonds = pow (0.5 + 0.5 * cos (u * 31.4), 8.0) * u_core;

	float i = body * (fade * flicker * mix (0.5, 0.9, u_core) + 0.8 * diamonds);
	gl_FragColor = vec4 (mix (outer, core, u_core) * i, 1.0);
}

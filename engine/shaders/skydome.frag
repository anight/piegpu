// skydome: Quake's two cloud layers (the back drifting 8 units a second, the
// front 16 and over it, see-through where it's clear: alpha 0), as a flat
// deck in perspective rather than Quake's flattened dome (whose clouds
// stretch out at the horizon): overhead as Quake's, smaller and smaller
// towards the horizon, mirrored below it; a haze of the back layer's own
// colour towards the horizon
precision mediump float;

uniform sampler2D u_back;
uniform sampler2D u_front;
uniform float u_time;			// seconds, mod 16 (both layers' period)
uniform vec3 u_haze;			// the back layer's colour, on average

varying vec3 v_dir;

void main ()
{
	vec3 n = v_dir / length (v_dir);
	vec2 st = n.xy * (6.0 * 63.0 * 1.1 / 3.0 / (abs (n.z) + 0.1));	// (Quake's scale overhead)
	vec3 back = texture2D (u_back, (st + u_time * 8.0) / 128.0).rgb;
	vec4 front = texture2D (u_front, (st + u_time * 16.0) / 128.0);
	float h = 1.0 - abs (n.z);
	gl_FragColor = vec4 (mix (mix (back, front.rgb, front.a), u_haze, h * h * h * 0.9), 1.0);
}

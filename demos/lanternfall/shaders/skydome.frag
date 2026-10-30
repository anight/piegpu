// skydome: an overcast night: the engine's two layers of cloud, drifting, out
// of the fog that everything far is lost in; lit a moment by lightning
precision mediump float;

uniform sampler2D u_back;
uniform sampler2D u_front;
uniform float u_time;			// seconds, mod 16 (both layers' period)
uniform vec4 u_fog;
uniform float u_storm;

varying vec3 v_dir;

void main ()
{
	vec3 n = v_dir / length (v_dir);
	vec2 st = n.xy * (6.0 * 63.0 * 1.1 / 3.0 / (abs (n.z) + 0.1));
	vec3 back = texture2D (u_back, (st + u_time * 8.0) / 128.0).rgb;	// (the layers are 64 texels: drawn twice as large)
	vec4 front = texture2D (u_front, (st + u_time * 16.0) / 128.0);
	float h = 1.0 - max (n.z, 0.0);
	vec3 c = mix (back, front.rgb, front.a) * (2.2 + 2.5 * u_storm) + vec3 (0.25, 0.35, 0.4) * u_storm;
	gl_FragColor = vec4 (mix (c, u_fog.rgb, min (h * h * 1.15, 1.0)), 1.0);
}

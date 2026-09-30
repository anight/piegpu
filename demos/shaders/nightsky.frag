// nightsky: the haze at the horizon to black overhead, and the stars: the
// star texture laid over the sky dome from above (a plane seen through the
// direction: stretched towards the horizon, where they fade out); in the sky's
// one pass, instead of a second one blended over it
precision mediump float;

uniform float u_horizon;	// the horizon's row (NDC y)
uniform vec3 u_haze;
uniform vec3 u_zenith;
uniform sampler2D u_stars;	// 64 x 128 texels: v at half the rate for square ones

varying vec2 v_ndc;
varying vec3 v_dir;

void main ()
{
	vec3 c = mix (u_haze, u_zenith, smoothstep (u_horizon, u_horizon + 1.1, v_ndc.y));
	vec3 d = normalize (v_dir);
	vec2 uv = d.xz / (d.y + 0.35) * vec2 (1.5, 0.75);
	c += texture2D (u_stars, uv).rgb * smoothstep (0.02, 0.25, d.y);
	gl_FragColor = vec4 (c, 1.0);
}

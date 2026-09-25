// sky: haze at the horizon to deep blue overhead, and the sun's glow
precision mediump float;

uniform float u_horizon;	// the horizon's row (NDC y)
uniform vec2 u_sun;		// the sun's position (NDC; far away when behind)
uniform float u_aspect;		// width / height
uniform vec3 u_haze;
uniform vec3 u_zenith;

varying vec2 v_ndc;

// ordered dither against the RGB565 panel's banding (interleaved gradient noise)
float dither ()
{
	return (fract (52.9829189 * fract (dot (gl_FragCoord.xy, vec2 (0.06711056, 0.00583715)))) - 0.5) / 31.0;
}

void main ()
{
	vec3 c = mix (u_haze, u_zenith, smoothstep (u_horizon, u_horizon + 1.1, v_ndc.y));
	vec2 d = (v_ndc - u_sun) * vec2 (u_aspect, 1.0);
	float r2 = dot (d, d);
	c += vec3 (1.0, 0.92, 0.75) * (1.3 * exp (-r2 * 400.0) + 0.35 * exp (-r2 * 12.0));
	gl_FragColor = vec4 (c + dither (), 1.0);
}

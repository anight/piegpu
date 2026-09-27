// deck: the cloud deck below: sea between the clouds, sunlit tops, haze far off
precision mediump float;

uniform sampler2D u_noise;
uniform vec3 u_haze;

varying vec2 v_uv1;
varying vec2 v_uv2;
varying vec3 v_view;

// ordered dither against the RGB565 panel's banding (interleaved gradient noise)
float dither ()
{
	return (fract (52.9829189 * fract (dot (gl_FragCoord.xy, vec2 (0.06711056, 0.00583715)))) - 0.5) / 31.0;
}

void main ()
{
	float big = texture2D (u_noise, v_uv1).r, small = texture2D (u_noise, v_uv2).r;
	float density = 0.72 * big + 0.28 * small;
	float cover = smoothstep (0.40, 0.62, density);
	vec3 sea = vec3 (0.07, 0.20, 0.36);
	vec3 cloud = mix (vec3 (0.55, 0.62, 0.74), vec3 (1.0, 0.98, 0.94), smoothstep (0.48, 0.80, density + 0.25 * (small - 0.5)));
	vec3 c = mix (sea, cloud, cover);
	float fog = 1.0 - exp (-length (v_view) * 0.0055);
	gl_FragColor = vec4 (mix (c, u_haze, fog) + dither (), 1.0);
}

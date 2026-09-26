// toy clouds: domain-warped fractal noise (fbm of fbm, after Inigo Quilez's
// "warping" article), drifting. Value noise from iChannel0 (random texels,
// bilinear): one fetch per octave, smoothed by moving the sample point
// within the texel (Quilez's trick), in place of four hashes.
precision mediump float;

uniform float iTime;
uniform vec2 iResolution;
uniform sampler2D iChannel0;	// 64x64 random RGBA, repeat, linear (toy.c)
varying vec2 v_coord;

float noise (vec2 p)
{
	vec2 i = floor (p);
	vec2 f = fract (p);
	f = f * f * (3.0 - 2.0 * f);
	return texture2D (iChannel0, (i + f + 0.5) / 64.0).x;
}

float fbm (vec2 p)
{
	float v = 0.0;
	float a = 0.5;
	for (int i = 0; i < 4; i++)
	{
		v += a * noise (p);
		p = mat2 (1.6, 1.2, -1.2, 1.6) * p;
		a *= 0.5;
	}
	return v;
}

void main ()
{
	vec2 p = v_coord / iResolution.y * 3.0;
	float t = 0.15 * iTime;
	vec2 q = vec2 (fbm (p + vec2 (0.0, t)), fbm (p + vec2 (5.2, 1.3)));
	vec2 r = vec2 (fbm (p + 4.0 * q + vec2 (1.7, 9.2) + t), fbm (p + 4.0 * q + vec2 (8.3, 2.8) - 0.8 * t));
	float f = fbm (p + 4.0 * r);

	vec3 col = mix (vec3 (0.10, 0.12, 0.35), vec3 (0.75, 0.35, 0.55), clamp (f * f * 4.0, 0.0, 1.0));
	col = mix (col, vec3 (0.05, 0.3, 0.4), clamp (length (q), 0.0, 1.0));
	col = mix (col, vec3 (1.0, 0.85, 0.6), clamp (r.x * r.x, 0.0, 1.0));
	col *= f * 1.6 + 0.3;
	gl_FragColor = vec4 (col, 1.0);
}

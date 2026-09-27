// toy tunnel: the classic polar tunnel - tiles on the walls, coloured by a
// cosine palette (after Inigo Quilez), fading into the distance
precision mediump float;

uniform float iTime;
uniform vec2 iResolution;
varying vec2 v_coord;

vec3 palette (float t)
{
	return vec3 (0.5) + vec3 (0.5) * cos (6.28318 * (vec3 (1.0) * t + vec3 (0.0, 0.33, 0.67)));
}

void main ()
{
	vec2 p = (2.0 * v_coord - iResolution) / iResolution.y;
	// the tunnel sways
	p += 0.15 * vec2 (sin (iTime * 0.5), cos (iTime * 0.37));
	float r = length (p);
	float a = atan (p.y, p.x);

	// 16 tiles around (a whole number, so the rows meet where atan wraps from
	// pi to -pi), about square: a tile is as deep as it is wide
	vec2 uv = vec2 (2.5 / r + 1.5 * iTime, a * (16.0 / 6.28318) + 0.3 * iTime);
	vec2 cell = floor (uv);
	vec2 g = abs (fract (uv) - 0.5);
	float edge = max (g.x, g.y);
	float tile = smoothstep (0.47, 0.40, edge);

	// colour by ring and by column (modulo 16: the same across the wrap)
	vec3 col = palette (0.07 * cell.x + mod (cell.y, 16.0) / 16.0 + 0.05 * iTime) * tile;
	col += vec3 (0.9, 0.6, 1.0) * smoothstep (0.40, 0.49, edge) * 0.35;	// glowing grout
	col *= smoothstep (0.05, 0.9, r);					// dark far away
	col += vec3 (1.0, 0.8, 0.6) * 0.05 / (r + 0.05);			// light at the end
	gl_FragColor = vec4 (col, 1.0);
}

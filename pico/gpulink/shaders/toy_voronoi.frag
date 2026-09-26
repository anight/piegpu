// toy voronoi: cells whose centres circle about, coloured by cell, with
// borders at the true distance to the nearest edge (two passes, after Inigo
// Quilez's "voronoi edges"). A cell's random numbers are a texel of
// iChannel0 (read at its centre), and the centres swing with a parabolic
// sine: both far cheaper on the V3D than a hash and sin in arithmetic.
precision mediump float;

uniform float iTime;
uniform vec2 iResolution;
uniform sampler2D iChannel0;	// 64x64 random RGBA, repeat, linear (toy.c)
varying vec2 v_coord;

vec2 hash2 (vec2 cell)
{
	return texture2D (iChannel0, (cell + 0.5) / 64.0).xy;
}

// sin (2 pi x), to within 0.06: a parabola per half turn
vec2 wave (vec2 x)
{
	vec2 t = fract (x) - 0.5;
	return -4.0 * t * (1.0 - 2.0 * abs (t)) * 2.0;
}

// the centre wanders at most 0.3 from the middle of its cell: then no centre
// two cells away can be nearer than the pixel's own cell's (at least 1.2
// against at most 1.13), so the 3x3 search for the nearest one is exact
vec2 centre (vec2 cell)
{
	return 0.5 + 0.3 * wave (iTime * 0.13 + hash2 (cell));
}

void main ()
{
	vec2 p = v_coord / iResolution.y * 4.0 + vec2 (0.25 * iTime, 0.0);
	vec2 n = floor (p);
	vec2 f = fract (p);

	// the nearest centre, and the second nearest's distance (squared)
	vec2 mg = vec2 (0.0);
	vec2 mr = vec2 (0.0);
	float md = 8.0;
	float md2 = 8.0;
	for (int j = -1; j <= 1; j++)
	{
		for (int i = -1; i <= 1; i++)
		{
			vec2 g = vec2 (float (i), float (j));
			vec2 r = g + centre (n + g) - f;
			float d = dot (r, r);
			md2 = min (md2, max (d, md));
			if (d < md)
			{
				md = d;
				mr = r;
				mg = g;
			}
		}
	}

	// the distance to the nearest edge, where it matters: every edge is at
	// least half the gap between the nearest centre and any other, and no
	// other centre is nearer than the second nearest of the 3x3 or 1.2 (see
	// centre ()); beyond 0.048 there is no border to draw, so most pixels
	// skip the search. The nearest centre's neighbours can be two cells
	// from its own: 5x5 without its corners (with 3x3, 2% of the pixels got
	// a wrong edge at a wander of 0.45 - bent or missing borders; checked
	// against the full 5x5 by simulating this shader over 48 frames)
	// A cell is only read when its centre could be near enough: the centre
	// is within 0.3 of the middle of its cell, so a cell whose box of places
	// lies farther than the nearest centre + 0.096 has no edge within 0.048.
	float me = 1.0;
	float d1 = sqrt (md);
	if (min (sqrt (md2), 1.2) - d1 < 0.096)
	{
		me = 8.0;
		float reach = (d1 + 0.096) * (d1 + 0.096);
		for (int j = -2; j <= 2; j++)
		{
			for (int i = -2; i <= 2; i++)
			{
				vec2 g = mg + vec2 (float (i), float (j));
				vec2 box = max (abs (g + 0.5 - f) - 0.3, 0.0);
				if (abs (i) + abs (j) < 4 && dot (box, box) < reach)
				{
					vec2 r = g + centre (n + g) - f;
					if (dot (mr - r, mr - r) > 0.00001)
					{
						me = min (me, dot (0.5 * (mr + r), normalize (r - mr)));
					}
				}
			}
		}
	}

	vec2 id = hash2 (n + mg);
	vec3 col = 0.55 + 0.45 * cos (6.2831 * (id.x + vec3 (0.0, 0.33, 0.67)) + 0.5 * iTime);
	col *= 0.75 + 0.25 * smoothstep (0.0, 0.6, sqrt (md));		// shaded towards the edge
	col *= smoothstep (0.016, 0.048, me);				// dark borders
	col += vec3 (1.0) * (1.0 - smoothstep (0.0, 0.08, sqrt (md))) * 0.6;	// a bright centre
	gl_FragColor = vec4 (col, 1.0);
}

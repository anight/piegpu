// toy pong: neon Pong drawn by distance functions - two glowing paddles, a
// ball with a fading trail and a flash when hit, 7-segment scores, a dashed
// net and a faint grid, under scanlines and a vignette. The game is played on
// the Pico (pong.c), which passes its state in pixels (origin bottom left).
precision mediump float;

uniform float iTime;
uniform vec2 iResolution;
varying vec2 v_coord;

uniform vec2 uBall;		// centre
uniform vec2 uPaddles;		// the left and right paddle's centre y
uniform vec2 uScore;		// left, right (0 .. 9)
uniform float uFlash;		// seconds since the ball was last hit
uniform vec2 uTrail[6];		// the ball's earlier places, newest first

const float MARGIN = 14.0;	// paddle centre from the side
const vec2 PADDLE = vec2 (4.0, 24.0);	// half size
const float BALL = 4.0;		// radius

float box (vec2 p, vec2 b, float r)
{
	vec2 q = abs (p) - b + r;
	return length (max (q, 0.0)) + min (max (q.x, q.y), 0.0) - r;
}

float segment (vec2 p, vec2 a, vec2 b)
{
	vec2 pa = p - a;
	vec2 ba = b - a;
	return length (pa - ba * clamp (dot (pa, ba) / dot (ba, ba), 0.0, 1.0));
}

// the distance to a 7-segment digit d (0 .. 9) in a cell x -0.5 .. 0.5,
// y -1 .. 1; segments a .. g are bits 0 .. 6 of the mask
float digit (vec2 p, float d)
{
	float m =   d < 0.5 ? 63.0 : d < 1.5 ? 6.0 : d < 2.5 ? 91.0 : d < 3.5 ? 79.0 : d < 4.5 ? 102.0
		  : d < 5.5 ? 109.0 : d < 6.5 ? 125.0 : d < 7.5 ? 7.0 : d < 8.5 ? 127.0 : 111.0;
	float r = 9.0;
	if (mod (m, 2.0) > 0.5)                  r = min (r, segment (p, vec2 (-0.5, 1.0), vec2 (0.5, 1.0)));
	if (mod (floor (m / 2.0), 2.0) > 0.5)    r = min (r, segment (p, vec2 (0.5, 1.0), vec2 (0.5, 0.0)));
	if (mod (floor (m / 4.0), 2.0) > 0.5)    r = min (r, segment (p, vec2 (0.5, 0.0), vec2 (0.5, -1.0)));
	if (mod (floor (m / 8.0), 2.0) > 0.5)    r = min (r, segment (p, vec2 (-0.5, -1.0), vec2 (0.5, -1.0)));
	if (mod (floor (m / 16.0), 2.0) > 0.5)   r = min (r, segment (p, vec2 (-0.5, 0.0), vec2 (-0.5, -1.0)));
	if (mod (floor (m / 32.0), 2.0) > 0.5)   r = min (r, segment (p, vec2 (-0.5, 1.0), vec2 (-0.5, 0.0)));
	if (mod (floor (m / 64.0), 2.0) > 0.5)   r = min (r, segment (p, vec2 (-0.5, 0.0), vec2 (0.5, 0.0)));
	return r;
}

// a solid core and a soft glow around a shape at distance d (pixels)
vec3 neon (float d, vec3 c)
{
	return c * (smoothstep (1.0, -1.0, d) + 0.55 * exp (-max (d, 0.0) * 0.18));
}

void main ()
{
	vec2 p = v_coord;
	vec3 col = vec3 (0.015, 0.01, 0.04);

	// the grid and the net
	vec2 g = abs (fract (p / 20.0) - 0.5);
	col += vec3 (0.03, 0.02, 0.07) * smoothstep (0.45, 0.5, max (g.x, g.y));
	float net = abs (p.x - 0.5 * iResolution.x) - 1.0;
	col += vec3 (0.25, 0.25, 0.55) * step (0.5, fract (p.y / 14.0)) * smoothstep (1.0, 0.0, net);

	// the scores: 7-segment digits either side of the net
	// (only near them: most pixels skip the digits)
	float cs = 11.0;		// digit half height, pixels
	vec2 top = vec2 (0.5 * iResolution.x, iResolution.y - 28.0);
	vec2 near = abs (p - top);
	if (near.y < 30.0 && near.x < 60.0)
	{
		float dl = digit ((p - top - vec2 (-34.0, 0.0)) / cs, uScore.x) * cs - 1.5;
		float dr = digit ((p - top - vec2 (34.0, 0.0)) / cs, uScore.y) * cs - 1.5;
		col += 0.8 * neon (dl, vec3 (0.2, 0.8, 1.0)) + 0.8 * neon (dr, vec3 (1.0, 0.3, 0.7));
	}

	// the paddles
	col += neon (box (p - vec2 (MARGIN, uPaddles.x), PADDLE, 3.0), vec3 (0.2, 0.8, 1.0));
	col += neon (box (p - vec2 (iResolution.x - MARGIN, uPaddles.y), PADDLE, 3.0), vec3 (1.0, 0.3, 0.7));

	// the trail (only near it), then the ball (brighter just after a hit)
	// (a box round all its points: after a bounce the trail bends at the wall)
	vec2 lo = uTrail[0];
	vec2 hi = uTrail[0];
	for (int i = 1; i < 6; i++)
	{
		lo = min (lo, uTrail[i]);
		hi = max (hi, uTrail[i]);
	}
	vec2 span = max (lo - p, p - hi);
	if (max (span.x, span.y) < BALL + 2.0)
	{
		for (int i = 0; i < 6; i++)
		{
			float k = 1.0 - float (i + 1) / 7.0;
			float d = length (p - uTrail[i]) - BALL * k;
			col += vec3 (1.0, 0.6, 0.2) * 0.35 * k * smoothstep (1.0, -1.0, d);
		}
	}
	float flash = 1.0 + 2.5 * exp (-uFlash * 8.0);
	col += neon (length (p - uBall) - BALL, vec3 (1.0, 0.9, 0.55)) * flash;

	// the tube: scanlines, vignette, gentle tone map
	col *= 0.8 + 0.2 * step (0.5, fract (p.y * 0.5));
	vec2 v = p / iResolution - 0.5;
	col *= 1.0 - 0.7 * dot (v, v);
	col = 1.0 - exp (-1.6 * col);
	// dither half a step of the panel's RGB565 (5, 6, 5 bits): near black a
	// single step is visible, and smooth fades would show its contours
	float nz = fract (52.9829 * fract (dot (p, vec2 (0.06711056, 0.00583715))));
	col += (nz - 0.5) * vec3 (1.0 / 32.0, 1.0 / 64.0, 1.0 / 32.0);
	gl_FragColor = vec4 (col, 1.0);
}

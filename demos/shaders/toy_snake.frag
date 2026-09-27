// toy snake: neon Snake on a 32 x 24 grid of 10-pixel cells. The Pico plays
// (snake.c) and sends the grid as a texture, a texel per cell: R the body
// (1 at the head, falling towards the tail), G the food, B the head, A the
// segment's number (modulo 256: neighbours in the snake differ by 1). Here
// each cell is drawn with its four neighbours (so the glow crosses cells
// and segments join up), the food pulses, and the board flashes when the
// snake dies.
precision mediump float;

uniform float iTime;
uniform vec2 iResolution;
varying vec2 v_coord;

uniform sampler2D uGrid;	// 32 x 24, nearest (snake.c)
uniform float uDead;		// seconds since the snake died (large: alive)
uniform vec2 uScore;		// the length: tens, units

const vec2 GRID = vec2 (32.0, 24.0);
const float CELL = 10.0;

vec4 cell (vec2 c)
{
	if (c.x < 0.0 || c.y < 0.0 || c.x >= GRID.x || c.y >= GRID.y)
	{
		return vec4 (0.0);
	}
	return texture2D (uGrid, (c + 0.5) / GRID);
}

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

void main ()
{
	vec2 p = v_coord;
	vec2 c = floor (p / CELL);
	vec2 f = p - (c + 0.5) * CELL;		// pixels from the cell's centre

	vec3 col = vec3 (0.01, 0.015, 0.03);
	col += vec3 (0.05, 0.06, 0.12) * smoothstep (1.2, 0.4, length (f));	// a dot per cell

	// the body: this cell and its neighbours, each a rounded square, with a
	// bridge to a neighbour that is the next or previous segment
	float body = 99.0;
	float shade = 0.0;
	float head = 99.0;
	float food = 99.0;
	vec4 me = cell (c);
	for (int k = 0; k < 5; k++)
	{
		vec2 o = k == 0 ? vec2 (0.0) : k == 1 ? vec2 (1.0, 0.0) : k == 2 ? vec2 (-1.0, 0.0)
			 : k == 3 ? vec2 (0.0, 1.0) : vec2 (0.0, -1.0);
		vec4 n = k == 0 ? me : cell (c + o);
		vec2 q = f - o * CELL;
		if (n.r > 0.0)
		{
			float d = box (q, vec2 (3.6), 1.8);
			float step = abs (n.a - me.a) * 255.0;
			if (k > 0 && me.r > 0.0 && (abs (step - 1.0) < 0.5 || abs (step - 255.0) < 0.5))
			{
				d = min (d, box (f - o * CELL * 0.5, vec2 (3.6) + abs (o) * CELL * 0.5, 1.0));
			}
			if (d < body)
			{
				body = d;
				shade = n.r;
			}
		}
		if (n.b > 0.5)
		{
			head = min (head, box (q, vec2 (4.2), 2.0));
		}
		if (n.g > 0.5)
		{
			food = min (food, length (q) - 3.0 - 0.8 * sin (iTime * 8.0));
		}
	}

	vec3 skin = mix (vec3 (0.1, 0.9, 0.5), vec3 (0.1, 0.5, 1.0), 1.0 - shade);
	// glows fade within about half a cell: the diagonal neighbours, not
	// looked at, would cut off a wider one
	col += skin * (smoothstep (0.8, -0.8, body) * 0.9 + 0.5 * exp (-min (max (body, 0.0), 20.0) * 0.5));
	col += vec3 (0.9, 1.0, 0.9) * (smoothstep (0.8, -0.8, head) * 0.5 + 0.3 * exp (-min (max (head, 0.0), 20.0) * 0.5));
	col += vec3 (1.0, 0.3, 0.2) * (smoothstep (0.8, -0.8, food) + 0.8 * exp (-min (max (food, 0.0), 20.0) * 0.45));

	// the score (the snake's length), top left: only near it
	float cs = 7.0;
	vec2 at = vec2 (16.0, iResolution.y - 16.0);
	if (p.x < 48.0 && p.y > iResolution.y - 34.0)
	{
		float dg = min (digit ((p - at) / cs, uScore.x), digit ((p - at - vec2 (16.0, 0.0)) / cs, uScore.y)) * cs - 1.2;
		col += vec3 (0.8, 0.9, 1.0) * (smoothstep (0.8, -0.8, dg) * 0.7 + 0.25 * exp (-min (max (dg, 0.0), 20.0) * 0.3));
	}

	// death: a red flash, fading (the V3D's exp is not 0 far below -100:
	// keep the argument small)
	col += vec3 (0.6, 0.05, 0.05) * exp (-min (uDead, 5.0) * 3.0);

	vec2 v = p / iResolution - 0.5;
	col *= 1.0 - 0.6 * dot (v, v);
	col = 1.0 - exp (-1.5 * col);
	// dither half a step of the panel's RGB565 (5, 6, 5 bits): near black a
	// single step is visible, and smooth fades would show its contours
	float nz = fract (52.9829 * fract (dot (p, vec2 (0.06711056, 0.00583715))));
	col += (nz - 0.5) * vec3 (1.0 / 32.0, 1.0 / 64.0, 1.0 / 32.0);
	gl_FragColor = vec4 (col, 1.0);
}

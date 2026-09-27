// toy asteroids: vector-display Asteroids - glowing outlines of a ship and of
// jagged spinning rocks (9 corners at random radii), shots, an explosion's
// sparks, a score, over twinkling stars. The Pico plays (asteroids.c) and
// passes the state in pixels (origin bottom left). Every part is only
// traced near where it is (a rock near its bounding circle, the ship, the
// sparks, the score), so most pixels skip most of the work.
precision mediump float;

uniform float iTime;
uniform vec2 iResolution;
varying vec2 v_coord;

uniform vec4 uShip;		// x, y, 1 = shown, -
uniform vec2 uHeading;		// cos, sin of the ship's heading
uniform float uThrust;		// 1 while the engine burns
uniform vec4 uRocks[10];	// x, y, radius (0: none), shape seed
uniform vec2 uShots[6];		// off screen when unused
uniform vec3 uBoom;		// the last explosion: x, y, age in seconds
uniform float uScore;

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

// a thin glowing line at distance d
float line (float d)
{
	return smoothstep (1.2, 0.2, d) + 0.35 * exp (-min (d, 40.0) * 0.3);
}

void main ()
{
	vec2 p = v_coord;
	vec3 col = vec3 (0.0, 0.0, 0.02);

	// stars: one in some 8-pixel cells, twinkling
	vec2 sc = floor (p / 8.0);
	vec3 h3 = fract (vec3 (sc.xyx) * 0.1031);
	h3 += dot (h3, h3.yzx + 33.33);
	float h = fract ((h3.x + h3.y) * h3.z);
	if (h > 0.93)
	{
		vec2 r = fract (h3.yz);			// (h3 itself is some 33 by now)
		vec2 sp = (sc + 0.5 + 0.6 * (r - 0.5)) * 8.0;
		float tw = 0.5 + 0.5 * sin (iTime * (1.0 + 3.0 * r.x) + 30.0 * h);
		float d = length (p - sp);
		col += vec3 (0.7, 0.8, 1.0) * (smoothstep (1.6, 0.3, d) + 0.25 * exp (-d * 1.2)) * (0.35 + 0.65 * tw);
	}

	// rocks
	float rock = 99.0;
	for (int i = 0; i < 10; i++)
	{
		vec4 r = uRocks[i];
		vec2 v = p - r.xy;
		float reach = r.z * 1.3 + 10.0;
		if (r.z > 0.0 && dot (v, v) < reach * reach)
		{
			// the straight edge between the corners before and after this
			// angle (the rock turning slowly, each its own way)
			float spin = iTime * 0.25 * (r.w - 3.14) / 3.14;
			float a = fract ((atan (v.y, v.x) - spin) / 6.28318) * 8.0;
			float k = floor (a);
			float k1 = mod (k + 1.0, 8.0);
			float r0 = r.z * (0.6 + 0.4 * fract (sin ((k + r.w * 7.0) * 12.9898) * 43758.55));
			float r1 = r.z * (0.6 + 0.4 * fract (sin ((k1 + r.w * 7.0) * 12.9898) * 43758.55));
			float a0 = k * 0.785398 + spin;
			float a1 = a0 + 0.785398;
			rock = min (rock, segment (v, r0 * vec2 (cos (a0), sin (a0)), r1 * vec2 (cos (a1), sin (a1))));
		}
	}
	col += vec3 (0.85, 0.9, 1.0) * line (rock);

	// the ship, in its own frame: nose along +x
	vec2 sv = p - uShip.xy;
	if (uShip.z > 0.5 && dot (sv, sv) < 28.0 * 28.0)
	{
		vec2 v = sv;
		float c = uHeading.x;
		float s = uHeading.y;
		vec2 q = vec2 (c * v.x + s * v.y, -s * v.x + c * v.y);
		float d = segment (q, vec2 (10.0, 0.0), vec2 (-7.0, 6.5));
		d = min (d, segment (q, vec2 (-7.0, 6.5), vec2 (-4.0, 0.0)));
		d = min (d, segment (q, vec2 (-4.0, 0.0), vec2 (-7.0, -6.5)));
		d = min (d, segment (q, vec2 (-7.0, -6.5), vec2 (10.0, 0.0)));
		col += vec3 (0.4, 1.0, 0.8) * line (d);
		if (uThrust > 0.5)
		{
			float flick = 11.0 + 4.0 * sin (iTime * 60.0);
			float fl = min (segment (q, vec2 (-5.0, 3.0), vec2 (-flick, 0.0)),
					segment (q, vec2 (-5.0, -3.0), vec2 (-flick, 0.0)));
			col += vec3 (1.0, 0.55, 0.2) * line (fl);
		}
	}

	// shots: the nearest one's glow
	float shot = 99.0;
	for (int i = 0; i < 6; i++)
	{
		shot = min (shot, length (p - uShots[i]));
	}
	shot -= 1.2;
	col += vec3 (1.0, 0.9, 0.6) * (smoothstep (1.0, -0.5, shot) + 0.5 * exp (-min (max (shot, 0.0), 30.0) * 0.5));

	// the explosion: twelve sparks flying out, fading (at most 70 pixels a
	// second from where it started)
	vec2 bv = p - uBoom.xy;
	float br = 70.0 * uBoom.z + 8.0;
	if (uBoom.z < 1.5 && dot (bv, bv) < br * br)
	{
		float fade = 1.0 - uBoom.z / 1.5;
		for (int k = 0; k < 12; k++)
		{
			float a = float (k) * 0.5236 + 0.3 * sin (float (k) * 7.0);
			float speed = 45.0 + 25.0 * fract (float (k) * 0.618);
			vec2 sp = uBoom.xy + vec2 (cos (a), sin (a)) * speed * uBoom.z;
			float d = length (p - sp) - 1.0;
			col += vec3 (1.0, 0.7, 0.3) * fade * (smoothstep (1.0, -0.5, d) + 0.4 * exp (-max (d, 0.0) * 0.4));
		}
	}

	// the score, top left: five digits (only there)
	if (p.x < 90.0 && p.y > iResolution.y - 30.0)
	{
		float cs = 6.0;
		float ds = 99.0;
		for (int k = 0; k < 5; k++)
		{
			float place = k == 0 ? 10000.0 : k == 1 ? 1000.0 : k == 2 ? 100.0 : k == 3 ? 10.0 : 1.0;
			vec2 at = vec2 (14.0 + 14.0 * float (k), iResolution.y - 14.0);
			ds = min (ds, digit ((p - at) / cs, mod (floor (uScore / place), 10.0)) * cs);
		}
		col += vec3 (0.85, 0.9, 1.0) * line (ds) * 0.8;
	}

	vec2 v = p / iResolution - 0.5;
	col *= 1.0 - 0.5 * dot (v, v);
	col = 1.0 - exp (-1.4 * col);
	// dither half a step of the panel's RGB565 (5, 6, 5 bits): near black a
	// single step is visible, and smooth fades would show its contours
	float nz = fract (52.9829 * fract (dot (p, vec2 (0.06711056, 0.00583715))));
	col += (nz - 0.5) * vec3 (1.0 / 32.0, 1.0 / 64.0, 1.0 / 32.0);
	gl_FragColor = vec4 (col, 1.0);
}

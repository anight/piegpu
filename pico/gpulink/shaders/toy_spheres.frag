// toy spheres: a raymarched scene - a spinning torus melting into an orbiting
// sphere (smooth min) over a checkered floor, with sun light, specular,
// ambient occlusion and fog. Signed distance functions after Inigo Quilez.
// Only the shapes are marched, and only inside their bounding sphere; the
// floor is a plane, hit exactly: most rays never march.
precision mediump float;

uniform float iTime;
uniform vec2 iResolution;
varying vec2 v_coord;

vec2 spin;		// cos, sin of the torus angle (set once in main)
vec3 orbit;		// the sphere's centre

float smin (float a, float b, float k)
{
	float h = clamp (0.5 + 0.5 * (b - a) / k, 0.0, 1.0);
	return mix (b, a, h) - k * h * (1.0 - h);
}

const float FLOOR = -0.9;
const vec3 CENTRE = vec3 (0.0, 0.1, 0.0);	// of the shapes' bounding sphere
const float BOUND = 2.0;			// its radius (reach 1.7 and the blend)

float shapes (vec3 p)
{
	vec3 q = p - CENTRE;
	q.xy = vec2 (spin.x * q.x - spin.y * q.y, spin.y * q.x + spin.x * q.y);
	q.yz = vec2 (spin.x * q.y + spin.y * q.z, -spin.y * q.y + spin.x * q.z);
	float torus = length (vec2 (length (q.xz) - 0.75, q.y)) - 0.25;
	float sphere = length (p - orbit) - 0.4;
	return smin (torus, sphere, 0.35);
}

float map (vec3 p)
{
	return min (p.y - FLOOR, shapes (p));
}

vec3 normal (vec3 p)
{
	const vec2 e = vec2 (0.002, -0.002);
	return normalize (  e.xyy * shapes (p + e.xyy) + e.yyx * shapes (p + e.yyx)
			  + e.yxy * shapes (p + e.yxy) + e.xxx * shapes (p + e.xxx));
}

void main ()
{
	spin = vec2 (cos (iTime * 0.9), sin (iTime * 0.9));
	orbit = vec3 (1.25 * sin (iTime * 0.7), 0.05 + 0.3 * sin (iTime * 1.3), 1.25 * cos (iTime * 0.7));

	vec2 p = (2.0 * v_coord - iResolution) / iResolution.y;
	float ca = 0.3 * iTime;
	vec3 ro = vec3 (3.2 * sin (ca), 1.1, 3.2 * cos (ca));
	vec3 fw = normalize (-ro);
	vec3 rt = normalize (cross (fw, vec3 (0.0, 1.0, 0.0)));
	vec3 up = cross (rt, fw);
	vec3 rd = normalize (p.x * rt + p.y * up + 1.6 * fw);

	// the floor, exactly; then the shapes, marched between where the ray
	// enters and leaves their bounding sphere (and not past the floor)
	float tmax = rd.y < 0.0 ? min ((FLOOR - ro.y) / rd.y, 12.0) : 12.0;
	float t = tmax;
	bool shape = false;
	vec3 oc = ro - CENTRE;
	float b = dot (oc, rd);
	float h = b * b - dot (oc, oc) + BOUND * BOUND;
	if (h > 0.0)
	{
		h = sqrt (h);
		float s = max (-b - h, 0.0);
		float s1 = min (-b + h, tmax);
		for (int i = 0; i < 40; i++)
		{
			float d = shapes (ro + s * rd);
			if (d < 0.002)
			{
				shape = true;
				break;
			}
			s += d;
			if (s > s1)
			{
				break;
			}
		}
		if (shape)
		{
			t = s;
		}
	}

	vec3 sky = mix (vec3 (0.55, 0.65, 0.85), vec3 (0.15, 0.2, 0.45), clamp (p.y + 0.3, 0.0, 1.0));
	vec3 col = sky;
	if (t < 12.0)
	{
		vec3 pos = ro + t * rd;
		vec3 n = shape ? normal (pos) : vec3 (0.0, 1.0, 0.0);
		vec3 sun = normalize (vec3 (0.6, 0.8, 0.4));
		vec3 mate;
		if (!shape)
		{
			vec2 c = floor (pos.xz * 1.5);
			mate = mix (vec3 (0.25), vec3 (0.6), mod (c.x + c.y, 2.0));
		}
		else
		{
			mate = mix (vec3 (0.9, 0.35, 0.1), vec3 (0.1, 0.5, 0.9),
				    smoothstep (-0.3, 0.3, dot (pos - orbit, pos - orbit) - 0.5));
		}
		float dif = clamp (dot (n, sun), 0.0, 1.0);
		float ao = clamp (map (pos + 0.25 * n) / 0.25, 0.0, 1.0);	// one-tap occlusion
		vec3 h = normalize (sun - rd);
		float spe = pow (clamp (dot (n, h), 0.0, 1.0), 24.0);
		col = mate * (0.25 * ao * vec3 (0.6, 0.7, 1.0) + 0.9 * dif * vec3 (1.0, 0.95, 0.85))
		      + 0.5 * spe * dif;
		col = mix (col, sky, 1.0 - exp (-0.02 * t * t));			// fog
	}
	col = pow (col, vec3 (0.4545));						// gamma
	gl_FragColor = vec4 (col, 1.0);
}

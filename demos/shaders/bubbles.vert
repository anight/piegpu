// aquarium: points in the water. Bubbles rise by themselves from where they
// start (a_seed: x, z, where in its rise each is, its size), wobbling, and go
// round; with no rise (u_motion.y = 0) the same points are pellets of food
// where the host put them (a_seed: x, y, z, size)
attribute vec4 a_seed;

uniform mat4 u_vp;
uniform vec4 u_motion;		// seconds; the rise's speed (0: none); its height; pixels at distance 1

varying float v_fade;

void main ()
{
	vec3 p = a_seed.xyz;
	float size = a_seed.w;
	v_fade = 1.0;
	if (u_motion.y > 0.0)
	{
		float h = fract (a_seed.z + u_motion.x * u_motion.y * (0.7 + 0.6 * a_seed.w));
		float y = h * u_motion.z;
		p = vec3 (a_seed.x + 0.10 * sin (y * 3.1 + a_seed.z * 40.0) * (0.5 + y * 0.3), y,
			  a_seed.y + 0.08 * cos (y * 2.3 + a_seed.z * 23.0) * (0.5 + y * 0.3));
		size = (0.5 + 0.5 * a_seed.w) * (0.7 + 0.6 * h);
		v_fade = min (h * 12.0, 1.0) * min ((1.0 - h) * 20.0, 1.0);
	}
	gl_Position = u_vp * vec4 (p, 1.0);
	gl_PointSize = max (u_motion.w * size / gl_Position.w, 1.0);
}

// jetwater: Jet's screen-space water (Renderer.cpp, WATER_REFLECT): per row, the
// frame mirrored about the waterline with a ripple denser towards the horizon,
// the sky gradient where the mirror leaves the scene, blended towards the
// water's colour by its alpha
precision highp float;

uniform sampler2D u_frame;	// the previous frame, the scene's rows (GL order)
uniform sampler2D u_sky;	// the gradient: a texel per row, the top first
uniform vec4 u_water;		// ripple amplitude (rows), phase (degrees), waterline, bias
uniform vec4 u_water2;		// last reflected row (-1: none), alpha, rows, the scene's top (GL y)
uniform vec4 u_color;		// the water's colour, width
uniform float u_sky_rows;	// 0: no gradient

void main ()
{
	float rows = u_water2.z, width = u_color.w;
	float y = floor (u_water2.w - gl_FragCoord.y);		// the scene row, 0 at the top
	float persp = rows - y;
	float angle = mod (floor (persp * persp * 20.0 / rows) + u_water.y, 360.0);
	float ripple = floor (floor (sin (radians (angle)) * 1024.0) * u_water.x / 1024.0);
	float wl = u_water.z;
	float mirror = 2.0 * wl - y + ripple + u_water.w;

	bool has_sky = u_sky_rows > 0.5;
	bool sky_only = mirror < 0.0 && has_sky;
	vec3 sky = texture2D (u_sky, vec2 (0.5, 0.5 / max (u_sky_rows, 1.0))).rgb;
	float scene = 1.0;
	if (u_water2.x >= 0.0 && !sky_only && has_sky)
	{
		float sky_row = clamp (2.0 * wl - y + ripple, 0.0, u_sky_rows - 1.0);
		sky = texture2D (u_sky, vec2 (0.5, (sky_row + 0.5) / u_sky_rows)).rgb;
		scene = clamp ((u_water2.x - mirror) / 24.0, 0.0, 1.0);
		sky_only = scene <= 0.0;
	}
	mirror = clamp (mirror, 0.0, rows - 1.0);
	float alpha = u_water2.y;
	if (mirror < 32.0)
	{
		alpha *= mirror / 32.0;
	}
	vec3 source = sky;
	if (!sky_only)
	{
		vec3 frame = texture2D (u_frame, vec2 (gl_FragCoord.x / width, (rows - mirror - 0.5) / rows)).rgb;
		source = mix (sky, frame, scene);
	}
	gl_FragColor = vec4 (mix (u_color.rgb, source, alpha), 1.0);
}

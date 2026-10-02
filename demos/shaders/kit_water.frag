// kit_water: Jet's screen-space water (its Renderer.cpp, WATER_REFLECT), per
// pixel: the picture above the water (drawn into a texture before) mirrored
// about the waterline, its rows shifted by a ripple that is denser towards
// the horizon; the sky's gradient where the mirror leaves the land; and all
// of it blended into the water's colour. Rows are the scene's (320, 0 the top)
precision highp float;

uniform sampler2D u_frame;	// the scene without the water
uniform sampler2D u_sky;	// the sky's gradient: a texel a row, the top first
uniform vec4 u_area;		// the scene's place: its left, its top (window coordinates); 1 / width, 320 / height
uniform vec4 u_water;		// the waterline's row; the shore's shift (rows); the last row reflected; the ripple's phase (degrees)
uniform vec4 u_color;		// the water's colour; how much it reflects

void main ()
{
	float y = (u_area.y - gl_FragCoord.y) * u_area.w - 0.5;
	float persp = 320.0 - y;
	float ripple = 4.0 * sin (radians (mod (persp * persp * (20.0 / 320.0) + u_water.w, 360.0)));
	float mirror = 2.0 * u_water.x - y + ripple;
	vec3 sky = texture2D (u_sky, vec2 (0.5, (clamp (mirror, 0.0, 319.0) + 0.5) / 320.0)).rgb;
	mirror += u_water.y;
	float land = clamp ((u_water.z - mirror) / 24.0, 0.0, 1.0);	// (below the shore's row there is only sea)
	mirror = clamp (mirror, 0.0, 319.0);
	float alpha = u_color.a * min (mirror / 32.0, 1.0);		// (fading out towards the picture's top)
	vec3 frame = texture2D (u_frame, vec2 ((gl_FragCoord.x - u_area.x) * u_area.z, 1.0 - (mirror + 0.5) / 320.0)).rgb;
	gl_FragColor = vec4 (mix (u_color.rgb, mix (sky, frame, land), alpha), 1.0);
}

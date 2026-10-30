// invaders: the one-bit screen (a texture, a texel a pixel, linear) seen
// through the cabinet's coloured cellophane: red over the saucer's line,
// green over the shields and the cannon, and the cannons left at the bottom.
// Scaled sharp: each pixel square, blended only across its edge's last
// screen pixel (fine at any size, the panel's 0.94 and HDMI's 2.3)
precision mediump float;

uniform sampler2D u_tex;
uniform vec4 u_size;		// 1 / the texture's width, 1 / its height; screen pixels a game pixel, across, down

varying vec2 v_pixel;

void main ()
{
	vec2 p = v_pixel - 0.5;
	vec2 base = floor (p);
	vec2 f = clamp ((p - base - 0.5) * u_size.zw + 0.5, 0.0, 1.0);
	float v = texture2D (u_tex, (base + 0.5 + f) * u_size.xy).r;
	vec3 tint = vec3 (1.0);
	if (v_pixel.y >= 32.0 && v_pixel.y < 64.0)
		tint = vec3 (1.0, 0.22, 0.22);
	else if ((v_pixel.y >= 184.0 && v_pixel.y < 240.0) || (v_pixel.y >= 240.0 && v_pixel.x < 136.0))
		tint = vec3 (0.25, 1.0, 0.35);
	gl_FragColor = vec4 (tint * v, 1.0);
}

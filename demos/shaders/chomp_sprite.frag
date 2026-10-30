// chomp: a sprite from the atlas (linear), scaled sharp as the maze is, tinted
precision mediump float;

uniform sampler2D u_atlas;
uniform vec4 u_size;		// 1 / the atlas's width, 1 / its height; screen pixels a game pixel, across, down

varying vec2 v_uv;
varying vec4 v_color;

void main ()
{
	vec2 p = v_uv - 0.5;
	vec2 b = floor (p);
	vec2 f = clamp ((p - b - 0.5) * u_size.zw + 0.5, 0.0, 1.0);
	vec4 t = texture2D (u_atlas, (b + 0.5 + f) * u_size.xy);
	if (t.a < 0.02)
		discard;
	gl_FragColor = vec4 (t.rgb * v_color.rgb, t.a * v_color.a);
}

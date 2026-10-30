// xonix: each cell from the field's texture (a texel a cell: 0 sea, 1 the
// trail, 2 .. 255 land, by the capture that made it): the sea dark, with a
// slow swell; the trail glowing; the land in tiles, lit from the top left;
// the cells just claimed flashing
precision mediump float;

uniform sampler2D u_tex;
uniform vec4 u_size;		// 1 / the texture's width, 1 / its height, the time, the flash (0 .. 1)
uniform float u_fresh;		// the cells just claimed: their value / 255

varying vec2 v_cell;

void main ()
{
	vec2 c = floor (v_cell);
	vec2 f = v_cell - c;
	float v = texture2D (u_tex, (c + 0.5) * u_size.xy).r * 255.0;
	vec3 col;
	if (v < 0.5)
	{
		float swell = 0.5 + 0.5 * sin (c.x * 0.45 + c.y * 0.30 + u_size.z * 1.7);
		col = vec3 (0.02, 0.05, 0.14) + vec3 (0.0, 0.025, 0.06) * swell;
	}
	else if (v < 1.5)
	{
		col = vec3 (1.0, 0.28, 0.66) * (0.82 + 0.18 * sin (u_size.z * 14.0));
	}
	else
	{
		float edge = step (f.x, 0.2) + step (f.y, 0.2) - step (0.8, f.x) - step (0.8, f.y);
		col = vec3 (0.05, 0.56, 0.62) * (1.0 + 0.24 * edge);
		if (abs (v - u_fresh * 255.0) < 0.5)
			col = mix (col, vec3 (1.0, 1.0, 0.85), u_size.w);
	}
	gl_FragColor = vec4 (col, 1.0);
}

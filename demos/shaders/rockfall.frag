// rockfall: each pixel through three lookups, all nearest: the map (a tile's
// number a cell: the cave, the status line in its last row), the atlas (a
// colour's number a pixel), the palette (the cave's own three colours and
// the fixed ones). Shown at a whole scale, so every pixel is the same size
precision mediump float;

uniform sampler2D u_map;	// 64 x 32
uniform sampler2D u_atlas;	// 256 x 256
uniform sampler2D u_pal;	// 16 x 1

varying vec2 v_pixel;

void main ()
{
	vec2 p = floor (v_pixel);
	vec2 cell = floor (p / 16.0);
	float t = floor (texture2D (u_map, (cell + 0.5) / vec2 (64.0, 32.0)).r * 255.0 + 0.5);
	vec2 a = vec2 (mod (t, 16.0), floor (t / 16.0)) * 16.0 + (p - cell * 16.0) + 0.5;
	float k = floor (texture2D (u_atlas, a / 256.0).r * 255.0 + 0.5);
	gl_FragColor = vec4 (texture2D (u_pal, vec2 ((k + 0.5) / 16.0, 0.5)).rgb, 1.0);
}

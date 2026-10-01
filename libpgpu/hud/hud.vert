// hud: text and bars in pixels (y down), coloured per vertex. A quad shows
// one glyph: which, a vertex attribute of its own (the host changes only
// that); a_corner is the texel within the glyph's 8x8 cell, 16 cells a row
// of the 128x32 font
attribute vec2 a_pos;
attribute vec2 a_corner;
attribute vec4 a_color;
attribute float a_glyph;

uniform vec2 u_scale;		// (2 / width, -2 / height)

varying vec2 v_uv;
varying vec4 v_color;

void main ()
{
	float row = floor ((a_glyph + 0.5) / 16.0);
	v_uv = (vec2 (a_glyph - 16.0 * row, row) * 8.0 + a_corner) / vec2 (128.0, 32.0);
	v_color = a_color;
	gl_Position = vec4 (a_pos * u_scale + vec2 (-1.0, 1.0), 0.0, 1.0);
}

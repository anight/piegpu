// chomp: the player, the ghosts, the fruit, the scores: quads from the atlas
attribute vec2 a_pos;
attribute vec2 a_uv;		// the atlas's pixels
attribute vec4 a_color;		// the tint (a ghost's body is white in the atlas)

varying vec2 v_uv;
varying vec4 v_color;

void main ()
{
	v_uv = a_uv;
	v_color = a_color;
	gl_Position = vec4 (a_pos, 0.0, 1.0);
}

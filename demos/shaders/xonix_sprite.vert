// xonix: the balls, the player, a ring: two triangles each, placed by the host
attribute vec2 a_pos;
attribute vec2 a_local;		// -1 .. 1 across it
attribute vec4 a_color;		// alpha: 1 a ball, 0.5 a ring, 0 a square

varying vec2 v_local;
varying vec4 v_color;

void main ()
{
	v_local = a_local;
	v_color = a_color;
	gl_Position = vec4 (a_pos, 0.0, 1.0);
}

// sky: a screen quad; the camera never rolls, so the sky depends on the row
attribute vec2 a_pos;		// normalized device coordinates

varying vec2 v_ndc;

void main ()
{
	v_ndc = a_pos;
	gl_Position = vec4 (a_pos, 0.0, 1.0);
}

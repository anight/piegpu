// rockfall: the cave's view and the status line, a quad each; a_pixel: where
// in the map they show, in its pixels (16 a cell)
attribute vec2 a_pos;
attribute vec2 a_pixel;

varying vec2 v_pixel;

void main ()
{
	v_pixel = a_pixel;
	gl_Position = vec4 (a_pos, 0.0, 1.0);
}

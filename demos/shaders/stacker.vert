// stacker: the screen, one quad; a_pixel: where on it, in the game's pixels
attribute vec2 a_pos;
attribute vec2 a_pixel;

varying vec2 v_pixel;

void main ()
{
	v_pixel = a_pixel;
	gl_Position = vec4 (a_pos, 0.0, 1.0);
}

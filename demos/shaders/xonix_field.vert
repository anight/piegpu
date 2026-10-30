// xonix: the field, one quad; a_cell: where on it, in cells
attribute vec2 a_pos;
attribute vec2 a_cell;

varying vec2 v_cell;

void main ()
{
	v_cell = a_cell;
	gl_Position = vec4 (a_pos, 0.0, 1.0);
}

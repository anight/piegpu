// full-screen background: position in clip space
attribute vec2 a_pos;
varying vec2 v_pos;

void main ()
{
	v_pos = a_pos * vec2 (1.333, 1.0);
	gl_Position = vec4 (a_pos, 0.999, 1.0);
}

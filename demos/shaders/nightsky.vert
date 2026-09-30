// nightsky: antigrav's sky at night, a screen quad (above the horizon); the
// view's direction at each corner comes from the host (interpolated: exact,
// the quad's w is 1)
attribute vec2 a_pos;		// normalized device coordinates
attribute vec3 a_dir;		// the world direction seen there

varying vec2 v_ndc;
varying vec3 v_dir;

void main ()
{
	v_ndc = a_pos;
	v_dir = a_dir;
	gl_Position = vec4 (a_pos, 0.99999, 1.0);	// at the far plane: drawn last, behind everything (antigrav)
}

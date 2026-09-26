// toy: a full-screen quad for the Shadertoy-style demos (toy.c); passes the
// pixel position as Shadertoy's fragCoord (pixels, origin bottom left)
attribute vec2 a_pos;		// clip space, -1 .. 1
uniform mediump vec2 iResolution;	// as the fragment shaders declare it
varying vec2 v_coord;

void main ()
{
	v_coord = (a_pos * 0.5 + 0.5) * iResolution;
	gl_Position = vec4 (a_pos, 0.0, 1.0);
}

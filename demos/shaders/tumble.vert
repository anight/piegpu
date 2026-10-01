// tumble: the bodies, two triangles each, placed by the host (the world's
// coordinates); a_local says where in the body's square a vertex is
attribute vec2 a_pos;
attribute vec2 a_local;		// -1 .. 1 across the body
attribute vec2 a_edge;		// a box: where its border starts, of 1, each way
attribute vec4 a_color;		// alpha 1: a ball, 0: a box

uniform vec4 u_view;		// the world's middle, and its scale to clip space

varying vec2 v_local;
varying vec2 v_edge;
varying vec4 v_color;

void main ()
{
	v_local = a_local;
	v_edge = a_edge;
	v_color = a_color;
	gl_Position = vec4 ((a_pos - u_view.xy) * u_view.zw, 0.0, 1.0);
}

// kit_sprite: the Jet scenes' 2D layer (demos/jet/kit.cpp): bitmaps and
// rectangles in the scene's pixels (480 x 320, y down)
attribute vec2 a_pos;
attribute vec2 a_uv;
attribute vec4 a_color;

uniform vec2 u_scale;		// 2 / 480, -2 / 320

varying vec2 v_uv;
varying vec4 v_color;

void main ()
{
	gl_Position = vec4 (a_pos * u_scale + vec2 (-1.0, 1.0), 0.0, 1.0);
	v_uv = a_uv;
	v_color = a_color;
}

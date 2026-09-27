// jet: what Jet queues (jet/gpu/JetGpu.cpp): triangles in screen pixels with
// camera-space depth, and 2D sprites
attribute vec4 a_pos;		// x, y (pixels, y down), z (camera units; 0 for 2D), texture weight (1024 = 1)
attribute vec2 a_uv;		// Jet's fixed point: 1024 = one texture
attribute vec4 a_color;		// untextured: the lit colour; textured: the light factor / 2; alpha

uniform vec2 u_scale;		// 2 / width, -2 / height

varying vec2 v_uv;
varying vec4 v_color;
varying float v_texture;

void main ()
{
	// w = the camera distance: textures and colours interpolate in perspective
	float w = a_pos.z > 0.0 ? a_pos.z : 1.0;
	gl_Position = vec4 ((a_pos.xy * u_scale + vec2 (-1.0, 1.0)) * w, 0.0, w);
	v_uv = a_uv / 1024.0;
	v_color = a_color;
	v_texture = a_pos.w / 1024.0;
}

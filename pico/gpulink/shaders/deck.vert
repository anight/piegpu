// deck: a horizontal cloud layer, a grid around the camera; positions are
// relative to the aircraft, texture coordinates anchored to the world
attribute vec2 a_grid;		// -1 .. 1

uniform mat4 u_vp;
uniform vec3 u_origin;		// the grid's centre: under or over the camera
uniform float u_extent;		// half size
uniform vec3 u_eye;
uniform vec4 u_scale;		// 1 / tile size of the two noise layers (x, y), unused (z, w)
uniform vec4 u_offset;		// the world position in tiles, wrapped: layer 1 (xy), layer 2 (zw)

varying vec2 v_uv1;
varying vec2 v_uv2;
varying vec3 v_view;		// from the eye

void main ()
{
	vec3 p = vec3 (u_origin.x + a_grid.x * u_extent, u_origin.y, u_origin.z + a_grid.y * u_extent);
	v_uv1 = p.xz * u_scale.x + u_offset.xy;
	v_uv2 = p.xz * u_scale.y + u_offset.zw;
	v_view = p - u_eye;
	gl_Position = u_vp * vec4 (p, 1.0);
}

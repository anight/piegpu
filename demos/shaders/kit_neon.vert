// kit_neon: the luminous tubes of the Jet scenes' "Neon film"
// (demos/jet/scenes/neon-film.cpp): a line between two points, drawn as a
// quad that always faces the camera, at least three pixels (of the scene's
// 480) wide however far it is, as the original turns them each frame; here
// the vertex shader does: a vertex knows its end, the other end, its side
attribute vec4 a_pos;		// this end
attribute vec4 a_normal;	// the other end (in 32767ths)
attribute vec4 a_color;
attribute vec2 a_uv;		// the side (-1, 1); the tube's width
attribute vec4 a_material;

uniform mat4 u_mvp;
uniform vec4 u_tint;
uniform mat4 u_view;
uniform vec4 u_ambient;
uniform vec4 u_lens;		// the projection's x and y scale; the focal length in pixels; the near plane

varying vec4 v_color;
varying vec3 v_light;
varying vec4 v_uvw;

void main ()
{
	vec3 other = floor (a_normal.xyz * 32767.0 + 0.5);
	vec3 a = (u_view * vec4 (a_pos.xyz, 1.0)).xyz, b = (u_view * vec4 (other, 1.0)).xyz;
	float za = max (a.z, u_lens.w), zb = max (b.z, u_lens.w);
	vec2 d = b.xy / zb - a.xy / za;
	float l = length (d);
	vec2 n = l > 0.00001 ? vec2 (-d.y, d.x) / l : vec2 (1.0, 0.0);
	float half_width = max (a_uv.y * 0.5, 1.5 * za / u_lens.z) * a_uv.x;
	gl_Position = u_mvp * vec4 (a_pos.xyz, 1.0) + vec4 (n * half_width * u_lens.xy, 0.0, 0.0);
	v_light = vec3 (1.0);
	v_color = vec4 (a_color.rgb * u_tint.rgb, a_color.a * u_ambient.a);
	v_uvw = vec4 (0.0, 0.0, 1.0, 1.0 + a_material.x * 0.0);
}

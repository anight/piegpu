// aquarium: the light that comes down through the water in shafts: sheets
// from the surface down, drawn over everything (added)
attribute vec3 a_pos;
attribute vec2 a_uv;		// across the sheet; down it (0 at the surface)

uniform mat4 u_vp;
uniform vec4 u_time;		// seconds; -

varying vec2 v_uv;
varying float v_shift;

void main ()
{
	v_uv = a_uv;
	v_shift = u_time.x * 0.012 + a_pos.z * 0.07;
	gl_Position = u_vp * vec4 (a_pos, 1.0);
}

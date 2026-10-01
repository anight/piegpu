// skydome: the sky's faces (engine/render.c: textures sky*); the direction from
// the eye to the fragment picks the clouds
attribute vec3 a_pos;

uniform mat4 u_vp;
uniform vec3 u_offset;
uniform vec3 u_eye;

varying vec3 v_dir;

void main ()
{
	vec3 p = a_pos + u_offset;
	v_dir = p - u_eye;
	gl_Position = u_vp * vec4 (p, 1.0);
}

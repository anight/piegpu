// gears: a gear in eye space, lit per pixel (gears.frag)
attribute vec3 a_pos;
attribute vec3 a_normal;

uniform mat4 u_modelview;
uniform mat4 u_projection;

varying vec3 v_normal;
varying vec3 v_eye_pos;

void main ()
{
	vec4 eye = u_modelview * vec4 (a_pos, 1.0);
	v_normal = (u_modelview * vec4 (a_normal, 0.0)).xyz;	// rotations only
	v_eye_pos = eye.xyz;
	gl_Position = u_projection * eye;
}

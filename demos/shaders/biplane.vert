// biplane: the mesh as stored (int16 positions, UVs and normals in 1/1024)
attribute vec3 a_pos;
attribute vec2 a_uv;
attribute vec3 a_normal;

uniform mat4 u_vp;
uniform mat4 u_model;		// scales to world units
uniform vec3 u_sun_dir;		// towards the sun

varying vec2 v_uv;
varying float v_light;

void main ()
{
	vec3 n = normalize ((u_model * vec4 (a_normal, 0.0)).xyz);
	v_light = 0.5 + 0.7 * max (dot (n, u_sun_dir), 0.0) + 0.15 * max (-n.y, 0.0);	// sun, light off the clouds
	v_uv = a_uv / 1024.0;
	gl_Position = u_vp * (u_model * vec4 (a_pos, 1.0));
}

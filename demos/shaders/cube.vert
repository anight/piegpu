// textured cube, lit per vertex by a point light (world space)
attribute vec3 a_pos;
attribute vec3 a_normal;
attribute vec2 a_uv;

uniform mat4 u_mvp;
uniform mat4 u_model;
uniform vec3 u_light_pos;
uniform vec3 u_light_color;
uniform vec3 u_eye;

varying vec2 v_uv;
varying vec3 v_diffuse;
varying vec3 v_specular;

void main ()
{
	vec3 world = (u_model * vec4 (a_pos, 1.0)).xyz;
	vec3 n = normalize ((u_model * vec4 (a_normal, 0.0)).xyz);
	vec3 l = normalize (u_light_pos - world);
	vec3 h = normalize (l + normalize (u_eye - world));

	v_diffuse = vec3 (0.18, 0.18, 0.22) + u_light_color * max (dot (n, l), 0.0);
	v_specular = u_light_color * pow (max (dot (n, h), 0.0), 24.0);
	v_uv = a_uv;
	gl_Position = u_mvp * vec4 (a_pos, 1.0);
}

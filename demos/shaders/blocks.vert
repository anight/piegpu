// blocks: lit, coloured geometry in world space (breakout)
attribute vec3 a_pos;
attribute vec3 a_normal;
attribute vec4 a_color;

uniform mat4 u_vp;		// projection * view
uniform mat4 u_model;
uniform vec3 u_light_dir;	// normalized, towards the light
uniform vec3 u_eye;		// the camera, world space
uniform float u_glow;		// emission, in units of the colour

varying vec3 v_color;

void main ()
{
	vec4 world = u_model * vec4 (a_pos, 1.0);
	vec3 n = normalize ((u_model * vec4 (a_normal, 0.0)).xyz);
	vec3 h = normalize (u_light_dir + normalize (u_eye - world.xyz));
	float diffuse = max (dot (n, u_light_dir), 0.0);
	float specular = pow (max (dot (n, h), 0.0), 24.0);

	v_color = a_color.rgb * (0.22 + 0.78 * diffuse + u_glow) + vec3 (0.45 * specular);
	gl_Position = u_vp * world;
}

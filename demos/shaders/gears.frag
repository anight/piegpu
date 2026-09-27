// gears: Blinn-Phong with a directional light (eye space)
precision mediump float;

uniform vec3 u_color;
uniform vec3 u_light_dir;	// normalized, towards the light

varying vec3 v_normal;
varying vec3 v_eye_pos;

void main ()
{
	vec3 n = normalize (v_normal);
	vec3 h = normalize (u_light_dir - normalize (v_eye_pos));
	float diffuse = max (dot (n, u_light_dir), 0.0);
	float specular = pow (max (dot (n, h), 0.0), 32.0);
	gl_FragColor = vec4 (u_color * (0.25 + 0.75 * diffuse) + vec3 (0.6 * specular), 1.0);
}

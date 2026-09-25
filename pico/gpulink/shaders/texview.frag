precision mediump float;

uniform sampler2D u_tex;
uniform samplerCube u_cube;
uniform float u_mode;		// 0 texture, 1 its RGB, 2 its alpha, 3 cube map in direction u_dir
uniform vec3 u_dir;
varying vec2 v_uv;

void main ()
{
	vec4 t = texture2D (u_tex, v_uv);
	if (u_mode < 0.5)
		gl_FragColor = t;
	else if (u_mode < 1.5)
		gl_FragColor = vec4 (t.rgb, 1.0);
	else if (u_mode < 2.5)
		gl_FragColor = vec4 (vec3 (t.a), 1.0);
	else
		gl_FragColor = textureCube (u_cube, u_dir);
}

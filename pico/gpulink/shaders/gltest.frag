// GL API test (gles/pgl.c)
precision mediump float;
uniform sampler2D u_tex;
uniform bool u_use_tex;
uniform vec4 u_tint;
varying vec2 v_uv;
varying vec4 v_color;

void main ()
{
	vec4 c = v_color * u_tint;
	if (u_use_tex)
		c *= texture2D (u_tex, v_uv);
	gl_FragColor = c;
}

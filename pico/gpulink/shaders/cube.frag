precision mediump float;

uniform sampler2D u_texture;
varying vec2 v_uv;
varying vec3 v_diffuse;
varying vec3 v_specular;

void main ()
{
	vec4 t = texture2D (u_texture, v_uv);
	gl_FragColor = vec4 (t.rgb * v_diffuse + v_specular, 1.0);
}

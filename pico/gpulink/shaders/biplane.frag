// biplane: the painted texture, lit per vertex
precision mediump float;

uniform sampler2D u_texture;

varying vec2 v_uv;
varying float v_light;

void main ()
{
	gl_FragColor = vec4 (texture2D (u_texture, v_uv).rgb * v_light, 1.0);
}

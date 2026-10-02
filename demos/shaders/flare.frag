// flare: the texture's light in the flare's colour, added to what's there
precision mediump float;

uniform sampler2D u_texture;

varying vec2 v_uv;
varying vec3 v_color;

void main ()
{
	gl_FragColor = vec4 (texture2D (u_texture, v_uv).rgb * v_color, 1.0);
}

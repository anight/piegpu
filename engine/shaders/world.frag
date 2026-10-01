// world: the texture times the lightmap, twice (Quake's overbright: 128 is
// the texture's own colour)
precision mediump float;

uniform sampler2D u_texture;
uniform sampler2D u_lightmap;

varying vec2 v_uv;
varying vec2 v_luv;

void main ()
{
	vec3 c = texture2D (u_texture, v_uv).rgb * (texture2D (u_lightmap, v_luv).r * 2.0);
	gl_FragColor = vec4 (c, 1.0);
}

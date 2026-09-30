// craft: the livery, lit; where its alpha is 0 it glows (the engines), unlit
precision mediump float;

uniform sampler2D u_texture;
uniform vec3 u_haze;
uniform float u_light;		// the scene's: day, night

varying vec2 v_uv;
varying float v_light;
varying float v_fog;

void main ()
{
	vec4 t = texture2D (u_texture, v_uv);
	vec3 c = t.rgb * mix (1.3, v_light * u_light, t.a);
	gl_FragColor = vec4 (mix (c, u_haze, v_fog), 1.0);
}

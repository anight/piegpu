// scenery: the texture, lit, into the haze. The texture's alpha says how
// much of it is lit: 1 by the light (the vertex's shade times the scene's
// light: day, night), 0 its own (it glows: the road's edges, the pads,
// screens, the lights); the shadows' alpha is their cover (their colour 0)
precision mediump float;

uniform sampler2D u_texture;
uniform vec3 u_haze;
uniform float u_light;

varying vec2 v_uv;
varying float v_shade;
varying float v_fog;

void main ()
{
	vec4 t = texture2D (u_texture, v_uv);
	vec3 c = mix (t.rgb * 1.15, t.rgb * (v_shade * u_light), t.a);
	gl_FragColor = vec4 (mix (c, u_haze, v_fog), t.a);	// (no dither: the textures' grain hides the banding)
}

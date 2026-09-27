// cirrus: wisps over the sky, fading into the haze (blended)
precision mediump float;

uniform sampler2D u_noise;

varying vec2 v_uv1;
varying vec2 v_uv2;
varying vec3 v_view;

void main ()
{
	float density = 0.6 * texture2D (u_noise, v_uv1).r + 0.4 * texture2D (u_noise, v_uv2).r;
	float fade = exp (-length (v_view) * 0.003);
	float a = smoothstep (0.52, 0.78, density) * 0.8 * fade;
	gl_FragColor = vec4 (vec3 (0.97, 0.97, 1.0), a);
}

// liquid: black water: the texture swaying slowly (as Quake's), dark; the
// lantern and the fires glint on it; then the fog
precision mediump float;

uniform sampler2D u_texture;
uniform vec2 u_size;			// the texture's, texels
uniform float u_time;			// seconds
uniform vec3 u_eye;
uniform vec4 u_lantern;
uniform vec3 u_gold;
uniform vec4 u_fire;
uniform vec3 u_fire_gold;
uniform vec4 u_fog;
uniform float u_storm;

varying vec2 v_uv;
varying vec3 v_pos;

void main ()
{
	vec2 t = v_uv * u_size;
	vec2 uv = v_uv + 8.0 * sin (t.yx * 0.125 + u_time * 0.6) / u_size;
	vec3 c = texture2D (u_texture, uv).rgb;
	vec3 light = vec3 (0.55, 0.85, 0.9) * (1.05 + u_storm);
	vec3 d = v_pos - u_lantern.xyz;
	float a = max (1.0 - dot (d, d) * u_lantern.w, 0.0);
	light += u_gold * (a * a) * 1.5;
	d = v_pos - u_fire.xyz;
	a = max (1.0 - dot (d, d) * u_fire.w, 0.0);
	light += u_fire_gold * (a * a) * 1.5;
	float f = min (length (v_pos - u_eye) * u_fog.w, 1.0);
	gl_FragColor = vec4 (mix (c * light, u_fog.rgb, f * f), 1.0);
}

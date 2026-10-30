// world: the paint (the texture) in the light there is. The level's own
// light (its lightmap, grey) is teal where it's dim and turns gold where it's
// bright: moonlight and lamplight from one number; and never none: the dark
// is a deep teal one can see in, not black. Two lights move: the
// lantern, and the nearest fire that burns (a shrine, a beacon); lightning
// adds to all of it. Then the fog. u_shown: a hidden way coming out of it.
precision mediump float;

uniform sampler2D u_texture;
uniform sampler2D u_lightmap;
uniform vec3 u_eye;
uniform vec4 u_lantern;		// where, and 1 / its reach squared
uniform vec3 u_gold;		// its colour, times its strength
uniform vec4 u_fire;
uniform vec3 u_fire_gold;
uniform vec4 u_fog;		// colour; 1 / the distance where it's all fog
uniform float u_storm;
uniform float u_shown;

varying vec2 v_uv;
varying vec2 v_luv;
varying vec3 v_pos;

void main ()
{
	vec3 t = texture2D (u_texture, v_uv).rgb;
	float l = texture2D (u_lightmap, v_luv).r * 2.0;
	vec3 light = mix (vec3 (0.50, 0.86, 0.92), vec3 (1.0, 0.76, 0.42), clamp (l * 1.2 - 0.45, 0.0, 1.0)) * (0.55 + 1.25 * l)
		     + vec3 (0.55, 0.75, 0.85) * u_storm;
	vec3 d = v_pos - u_lantern.xyz;
	float a = max (1.0 - dot (d, d) * u_lantern.w, 0.0);
	light += u_gold * (a * a);
	d = v_pos - u_fire.xyz;
	a = max (1.0 - dot (d, d) * u_fire.w, 0.0);
	light += u_fire_gold * (a * a);
	float f = min (length (v_pos - u_eye) * u_fog.w, 1.0);
	gl_FragColor = vec4 (mix (u_fog.rgb, mix (t * light, u_fog.rgb, f * f), u_shown), 1.0);
}

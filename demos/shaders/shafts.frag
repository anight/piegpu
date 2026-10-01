// aquarium: a sheet's streaks (a row of the caustics, stretched downwards,
// drifting), fading with the depth and towards the sheet's sides
precision mediump float;

uniform sampler2D u_caustics;
uniform vec4 u_color;		// the light's colour; how strong

varying vec2 v_uv;
varying float v_shift;

void main ()
{
	float streak = texture2D (u_caustics, vec2 (v_uv.x * 0.9 + v_shift, 0.31 + v_uv.y * 0.03)).r;
	streak *= texture2D (u_caustics, vec2 (v_uv.x * 0.6 - v_shift * 1.7, 0.72)).r;
	float down = 1.0 - v_uv.y;
	float side = v_uv.x * (1.0 - v_uv.x) * 4.0;
	float a = streak * down * down * side * u_color.a;
	gl_FragColor = vec4 (u_color.rgb * a, 1.0);
}

// aquarium: a bubble (a bright rim, its middle clear, a glint) or a pellet (a
// round brown grain); blended over the water
precision mediump float;

uniform vec4 u_color;		// alpha 0: a bubble, 1: a pellet

varying float v_fade;

void main ()
{
	vec2 q = gl_PointCoord - vec2 (0.5);
	float d = length (q) * 2.0;
	if (d > 1.0)
		discard;
	if (u_color.a > 0.5)
	{
		gl_FragColor = vec4 (u_color.rgb * (1.0 - 0.5 * d), 1.0);
	}
	else
	{
		float rim = smoothstep (0.55, 0.95, d);
		float glint = 1.0 - smoothstep (0.0, 0.28, length (q - vec2 (-0.16, -0.18)) * 2.0);
		gl_FragColor = vec4 (u_color.rgb, (0.10 + 0.55 * rim + 0.7 * glint) * v_fade);
	}
}

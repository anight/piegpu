// xonix: a ball (round, lit from the top left), a ring, or the player's square
// (a darker rim)
precision mediump float;

varying vec2 v_local;
varying vec4 v_color;

void main ()
{
	float r2 = dot (v_local, v_local);
	vec3 col = v_color.rgb;
	if (v_color.a > 0.75)
	{
		if (r2 > 1.0)
			discard;
		col *= 1.0 - 0.45 * r2 + 0.35 * clamp ((v_local.y - v_local.x) * 0.7, 0.0, 1.0);
	}
	else if (v_color.a > 0.25)
	{
		if (r2 > 1.0 || r2 < 0.72)
			discard;
	}
	else if (max (abs (v_local.x), abs (v_local.y)) > 0.66)
	{
		col *= 0.55;
	}
	gl_FragColor = vec4 (col, 1.0);
}

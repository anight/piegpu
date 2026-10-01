// tumble: a ball (round, a darker rim, a spoke that shows how it's turned) or
// a box (a darker border)
precision mediump float;

varying vec2 v_local;
varying vec2 v_edge;
varying vec4 v_color;

void main ()
{
	float shade = 1.0;
	if (v_color.a > 0.5)
	{
		float r2 = dot (v_local, v_local);
		if (r2 > 1.0)
			discard;
		if (r2 > 0.70)
			shade = 0.62;
		if (abs (v_local.y) < 0.13 && v_local.x > 0.0)
			shade = 0.45;
	}
	else
	{
		vec2 a = abs (v_local);
		if (a.x > v_edge.x || a.y > v_edge.y)
			shade = 0.62;
	}
	gl_FragColor = vec4 (v_color.rgb * shade, 1.0);
}

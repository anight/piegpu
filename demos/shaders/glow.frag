// glow: a soft disc (blended ONE, ONE)
precision mediump float;

varying vec4 v_color;

void main ()
{
	float d = length (gl_PointCoord - vec2 (0.5));
	float a = (1.0 - smoothstep (0.1, 0.5, d)) * v_color.a;
	gl_FragColor = vec4 (v_color.rgb * a, 1.0);
}

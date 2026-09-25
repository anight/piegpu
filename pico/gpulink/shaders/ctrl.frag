// control flow test (gltest.c): discard, and a loop whose trip count depends
// on the pixel
precision mediump float;
uniform float u_t;
uniform float u_mix;
varying vec4 v_c;

void main ()
{
	if (v_c.x < u_t)
		discard;
	float acc = 0.0;
	for (int i = 0; i < 32; i++)
	{
		if (float (i) >= mod (gl_FragCoord.x, 8.0))
			break;
		acc += 0.125;
	}
	gl_FragColor = vec4 (v_c.x, v_c.y, mix (v_c.z, acc, u_mix), v_c.w);
	if (u_mix > 1.5)
		gl_FragColor = vec4 (fract (gl_FragCoord.xy), 0.0, 1.0);	// pixel centres: 0.5
}

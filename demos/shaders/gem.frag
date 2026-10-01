// gem: the colour lit per vertex (each face flat)
precision mediump float;

varying vec3 v_color;

void main ()
{
	gl_FragColor = vec4 (v_color, 1.0);
}

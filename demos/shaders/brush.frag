// brush: the colour inside the square's circle
precision mediump float;

uniform vec4 u_color;
varying vec2 v_pos;

void main ()
{
	if (dot (v_pos, v_pos) > 1.0)
		discard;
	gl_FragColor = u_color;
}

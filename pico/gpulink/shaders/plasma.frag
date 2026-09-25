precision mediump float;

uniform float u_time;
uniform float u_brightness;
varying vec2 v_pos;

void main ()
{
	vec2 p = v_pos * 3.0;
	float v = sin (p.x + u_time)
		+ sin (p.y * 1.3 + u_time * 0.7)
		+ sin ((p.x + p.y) * 0.8 + u_time * 1.3)
		+ sin (length (p) * 1.5 - u_time);
	vec3 c = 0.5 + 0.5 * vec3 (sin (v), sin (v + 2.1), sin (v + 4.2));
	gl_FragColor = vec4 (c * u_brightness, 1.0);
}

// halo: brightest in the middle, nothing at the square's inscribed circle
precision mediump float;

uniform vec3 u_color;			// times the glow's strength

varying vec2 v_corner;

void main ()
{
	float r = 1.0 - min (dot (v_corner, v_corner), 1.0);
	gl_FragColor = vec4 (u_color * r * r, 1.0);
}

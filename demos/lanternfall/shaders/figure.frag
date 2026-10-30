// figure: the vertex's colour, into the fog
precision mediump float;

uniform vec4 u_fog;

varying vec3 v_color;
varying float v_fog;

void main ()
{
	gl_FragColor = vec4 (mix (v_color, u_fog.rgb, v_fog), 1.0);
}

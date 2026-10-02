// flare: zerog's lights (the engines, the shots, the explosions, a shield):
// squares facing the eye, their corners worked out by the host; dimmed by the
// haze between
attribute vec3 a_pos;
attribute vec2 a_uv;
attribute vec4 a_color;

uniform mat4 u_vp;
uniform vec3 u_eye;
uniform float u_fog;		// density, 1/m

varying vec2 v_uv;
varying vec3 v_color;

void main ()
{
	v_uv = a_uv;
	v_color = a_color.rgb * exp (-length (a_pos - u_eye) * u_fog);
	gl_Position = u_vp * vec4 (a_pos, 1.0);
}

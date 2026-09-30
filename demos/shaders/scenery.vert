// scenery: antigrav's track, walls, ground and props (and the craft's
// shadows): textured, lit per vertex by a shade the host works out (they
// don't move), fogged with the distance
attribute vec3 a_pos;
attribute vec2 a_uv;
attribute float a_shade;

uniform mat4 u_vp;
uniform mat4 u_model;		// identity but for the shadows
uniform vec3 u_eye;
uniform float u_fog;		// density, 1/m

varying vec2 v_uv;
varying float v_shade;
varying float v_fog;

void main ()
{
	vec4 p = u_model * vec4 (a_pos, 1.0);
	v_uv = a_uv;
	v_shade = a_shade;
	v_fog = 1.0 - exp (-length (p.xyz - u_eye) * u_fog);
	gl_Position = u_vp * p;
}

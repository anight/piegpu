// aquarium: the surface's colour, lit; the caustics (two layers of the same
// net of light, scrolled over each other: it shimmers); the water between
precision mediump float;

uniform sampler2D u_tex;
uniform sampler2D u_caustics;

varying vec2 v_uv;
varying vec4 v_cuv;
varying vec4 v_light;
varying vec3 v_fogc;

void main ()
{
	vec3 t = texture2D (u_tex, v_uv).rgb;
	float c = texture2D (u_caustics, v_cuv.xy).r * texture2D (u_caustics, v_cuv.zw).r;
	vec3 lit = t * (v_light.x + v_light.y * c * 3.2) + v_light.z;
	gl_FragColor = vec4 (mix (v_fogc, lit, v_light.w), 1.0);
}

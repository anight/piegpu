// kit_phong: a mesh lit per pixel (kit_phong.frag): the normal goes on, in
// view space; and the light worked out per vertex (kit_mesh.vert's), for a
// mesh whose brightness is only cut into cel bands there
attribute vec4 a_pos;
attribute vec4 a_normal;
attribute vec4 a_color;
attribute vec2 a_uv;
attribute vec4 a_material;	// diffuse, specular (0 .. 255); 1: lit; the gloss' exponent

uniform mat4 u_mvp;
uniform vec4 u_tint;		// over the vertices' colours (one mesh in several colours)
uniform mat4 u_view;
uniform mediump vec4 u_light;		// towards the light, view space; its intensity
uniform mediump vec4 u_tex;

varying vec4 v_color;
varying vec3 v_normal;
varying vec4 v_material;
varying vec3 v_uvw;
varying float v_brightness;

void main ()
{
	gl_Position = u_mvp * vec4 (a_pos.xyz, 1.0);
	vec3 n = normalize ((u_view * vec4 (a_normal.xyz, 0.0)).xyz);
	v_normal = n;
	v_color = vec4 (a_color.rgb * u_tint.rgb, a_color.a);
	v_material = a_material;
	float w = u_tex.y > 0.5 ? gl_Position.w : 1.0;
	v_uvw = vec3 (a_uv / 1024.0 * w, w);

	float b = 0.0, lit = dot (n, u_light.xyz);
	if (lit > 0.0)
	{
		float lambert = min (lit * 256.0, 255.0);
		lambert = lambert * lambert / 256.0 * u_light.w / 256.0;
		b = lambert * a_material.x / 256.0;
		if (a_material.y > 0.5 && n.z < 0.0)
		{
			float facing = min (-n.z * 256.0, 255.0);
			b += facing * facing / 256.0 * lambert / 256.0 * u_light.w / 256.0 * a_material.y / 256.0;
		}
		b = min (b, 255.0 + a_material.y);
	}
	v_brightness = b;
}

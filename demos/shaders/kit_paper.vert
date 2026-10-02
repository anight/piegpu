// kit_paper: the paper landscape of the Jet scenes' "Matter"
// (demos/jet/scenes/matter.cpp): a flat sheet of triangles lifted into
// waves here, on the GPU. A vertex knows its triangle's three corners (the
// normal's and the texture coordinates' places hold them), so that each
// corner works out the same folded face and its light (kit_mesh.vert's)
attribute vec4 a_pos;		// x, -, z
attribute vec4 a_normal;	// the triangle's first corner's x, z, the second's (in 32767ths)
attribute vec4 a_color;
attribute vec2 a_uv;		// the third's x, z
attribute vec4 a_material;	// diffuse, specular (0 .. 255); 1: lit; -

uniform mat4 u_mvp;
uniform vec4 u_tint;
uniform mat4 u_view;
uniform vec4 u_light;
uniform vec4 u_ambient;
uniform vec4 u_wave;		// seconds; -; -; -

varying vec4 v_color;
varying vec3 v_light;
varying vec4 v_uvw;

// the sheet's height: a wave along x (every other column of points up or down a little), one along z
float height (vec2 p)
{
	float column = floor (p.x / 100.0 + 12.5);
	float fold = mod (column, 2.0) > 0.5 ? 30.0 : -30.0;
	return -230.0 + 95.0 * sin (p.x * 0.006 + u_wave.x * 0.85) + fold + 50.0 * cos (p.y * 0.01 - u_wave.x * 0.65);
}

void main ()
{
	vec2 a = floor (a_normal.xy * 32767.0 + 0.5), b = floor (a_normal.zw * 32767.0 + 0.5), c = a_uv;
	vec3 pa = vec3 (a.x, height (a), a.y), pb = vec3 (b.x, height (b), b.y), pc = vec3 (c.x, height (c), c.y);
	gl_Position = u_mvp * vec4 (a_pos.x, height (a_pos.xz), a_pos.z, 1.0);
	vec3 t = vec3 (1.0);
	if (a_material.z > 0.5 && u_light.w >= 0.0)
	{
		vec3 n = normalize ((u_view * vec4 (cross (pb - pa, pc - pa), 0.0)).xyz);
		float most = 255.0 + a_material.y, bright = 0.0;
		float lit = dot (n, u_light.xyz);
		if (lit > 0.0)
		{
			float lambert = min (lit * 256.0, 255.0);
			lambert = lambert * lambert / 256.0 * u_light.w / 256.0;
			bright = min (lambert * a_material.x / 256.0, most);
		}
		t = min (vec3 (bright) + u_ambient.rgb, most) / 255.0;
	}
	v_light = t;
	v_color = vec4 (a_color.rgb * u_tint.rgb, a_color.a * u_ambient.a);
	v_uvw = vec4 (0.0, 0.0, 1.0, 1.0);
}

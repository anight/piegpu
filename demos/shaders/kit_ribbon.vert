// kit_ribbon: the ribbons of the Jet scenes' "Matter"
// (demos/jet/scenes/matter.cpp): a band wound about the z axis, turning
// in itself as it goes, shaped here, on the GPU, from where a vertex is along
// and across it; lit as kit_mesh.vert lights
attribute vec4 a_pos;		// how far along (0 .. the samples), across (-1, 0, 1), -
attribute vec4 a_normal;
attribute vec4 a_color;
attribute vec2 a_uv;
attribute vec4 a_material;	// diffuse, specular (0 .. 255); 1: lit; -

uniform mat4 u_mvp;
uniform vec4 u_tint;
uniform mat4 u_view;
uniform vec4 u_light;
uniform vec4 u_ambient;
uniform vec4 u_wave;		// seconds; the ribbon's phase; its length; half its width
uniform vec4 u_wave2;		// its samples; -; -; -

varying vec4 v_color;
varying vec3 v_light;
varying vec4 v_uvw;

void main ()
{
	float u = a_pos.x / u_wave2.x, a = u * 9.0 + u_wave.x * 0.7 + u_wave.y;
	vec3 side = vec3 (cos (a * 1.1), sin (a * 1.1), 0.0);
	vec3 tangent = vec3 (-2700.0 / u_wave.z * sin (a), 2430.0 / u_wave.z * cos (a), 1.0);
	vec3 p = vec3 (300.0 * cos (a), 270.0 * sin (a), (u - 0.5) * u_wave.z) + side * (a_pos.y * u_wave.w);
	gl_Position = u_mvp * vec4 (p, 1.0);
	vec3 t = vec3 (1.0);
	if (a_material.z > 0.5 && u_light.w >= 0.0)
	{
		vec3 n = normalize ((u_view * vec4 (cross (side, tangent), 0.0)).xyz);
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

// kit_phong: Jet's light per pixel (its Renderer.cpp, Specular.hpp): from the
// interpolated normal, the squared Lambert term and either the view-facing
// specular term or, for a glossy material (an exponent), a Blinn-Phong
// highlight added in the light's colour; the cel bands are the brightness
// with its low bits dropped; then the ambient per channel, and the blow-out
precision mediump float;

uniform sampler2D u_texture;
uniform vec4 u_tex;		// 1: textured; -; 1: colour key; -
uniform vec4 u_light;		// towards the light, view space; its intensity (0 .. 255)
uniform vec4 u_ambient;		// 0 .. 255 a channel; the mesh's alpha
uniform vec4 u_half;		// the half vector between the light and the eye (view space); 1: per pixel
				// (0: the vertices' brightness)
uniform vec4 u_gloss;		// the light's colour (0 .. 1); the cel step (1: none)

varying vec4 v_color;
varying vec3 v_normal;
varying vec4 v_material;
varying vec3 v_uvw;
varying float v_brightness;

void main ()
{
	vec3 base = v_color.rgb;
	if (u_tex.x > 0.5)
	{
		vec4 t = texture2D (u_texture, v_uvw.xy / v_uvw.z);
		if (u_tex.z > 0.5 && t.a < 0.5)
		{
			discard;
		}
		base = t.rgb;
	}
	float most = 255.0 + v_material.y, b = v_brightness, gloss = 0.0;
	if (u_half.w > 0.5)
	{
		vec3 n = normalize (v_normal);
		float lit = dot (n, u_light.xyz);
		b = 0.0;
		if (lit > 0.0)
		{
			float lambert = min (lit * 256.0, 255.0);
			lambert = lambert * lambert / 256.0 * u_light.w / 256.0;
			b = lambert * v_material.x / 256.0;
			if (v_material.w > 0.5 && v_material.y > 0.5)	// glossy: its highlight is the gloss alone
			{
				b = min (b, 255.0);
				float h = min (dot (n, u_half.xyz), 1.0);
				if (h > 0.0)
				{
					gloss = pow (h, v_material.w) * v_material.y * u_light.w / 255.0;
				}
			}
			else
			{
				if (v_material.y > 0.5 && n.z < 0.0)
				{
					float facing = min (-n.z * 256.0, 255.0);
					b += facing * facing / 256.0 * lambert / 256.0 * u_light.w / 256.0 * v_material.y / 256.0;
				}
				b = min (b, most);
			}
		}
	}
	b = floor (b / u_gloss.w) * u_gloss.w;
	vec3 t = v_material.z > 0.5 ? min (vec3 (b) + u_ambient.rgb, most) : vec3 (255.0);
	vec3 c = base * min (t, 255.0) / 255.0 + (1.0 - base) * max (t - 255.0, 0.0) / 256.0;
	gl_FragColor = vec4 (min (c + gloss / 255.0 * u_gloss.rgb, 1.0), v_color.a * u_ambient.a);
}

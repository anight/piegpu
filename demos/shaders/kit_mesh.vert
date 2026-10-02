// kit_mesh: a mesh of the Jet scenes (demos/jet/kit.cpp), lit per vertex
// the way Jet lights: brightness (0 .. 255 + specular) is a squared Lambert
// term times the light's intensity and the material's diffuse, plus a
// view-facing specular term; per channel the ambient is added
attribute vec4 a_pos;		// model space; w 1: plain (its colour, whatever the texture)
attribute vec4 a_normal;	// (xyz)
attribute vec4 a_color;		// the material's colour, alpha
attribute vec2 a_uv;		// 1024 = one texture
attribute vec4 a_material;	// diffuse, specular (0 .. 255); 1: lit; the gloss' exponent (kit_phong)

uniform mat4 u_mvp;
uniform vec4 u_tint;		// over the vertices' colours (one mesh in several colours)
uniform mat4 u_view;		// the model into view space (the camera looks along +z)
uniform vec4 u_light;		// towards the light, view space; its intensity (0 .. 255; below 0: no light)
uniform vec4 u_ambient;		// 0 .. 255 a channel; the mesh's alpha
uniform mediump vec4 u_tex;		// 1: textured; 1: texture coordinates without perspective; 1: colour key;
				// 1: outside the texture is colour 0
uniform vec4 u_lod;		// the texture fades into the colour from this distance to that (none if not further)

varying vec4 v_color;
varying vec3 v_light;		// per channel: 1 = the colour as it is, above: towards white
varying vec4 v_uvw;		// the coordinates (times w), w, how much of the texture (1 .. 0)

void main ()
{
	gl_Position = u_mvp * vec4 (a_pos.xyz, 1.0);
	vec3 t = vec3 (1.0);
	if (a_material.z > 0.5 && u_light.w >= 0.0)
	{
		vec3 n = normalize ((u_view * vec4 (a_normal.xyz, 0.0)).xyz);
		float most = 255.0 + a_material.y, b = 0.0;
		float lit = dot (n, u_light.xyz);
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
			b = min (b, most);
		}
		t = min (vec3 (b) + u_ambient.rgb, most) / 255.0;
	}
	v_light = t;
	v_color = vec4 (a_color.rgb * u_tint.rgb, a_color.a * u_ambient.a);
	// without perspective: the coordinates times w, divided by w again per pixel
	float w = u_tex.y > 0.5 ? gl_Position.w : 1.0;
	float fade = 1.0;
	if (u_lod.y > u_lod.x)
	{
		fade = clamp ((u_lod.y - gl_Position.w) / (u_lod.y - u_lod.x), 0.0, 1.0);
	}
	// (a vertex marked plain takes no texture: a mesh of both kinds in one draw)
	v_uvw = vec4 (a_uv / 1024.0 * w, w, fade * (1.0 - a_pos.w));
}

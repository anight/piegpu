// kit_mesh: the colour or the texel, times the light; above 1 a channel blows
// out towards white (Jet's jetModulateRGB565)
precision mediump float;

uniform sampler2D u_texture;
uniform vec4 u_tex;		// 1: textured; -; 1: colour key (alpha 0 texels are holes); 1: outside the texture is colour 0

varying vec4 v_color;
varying vec3 v_light;
varying vec4 v_uvw;

void main ()
{
	vec3 base = v_color.rgb;
	if (u_tex.x > 0.5)
	{
		vec2 uv = v_uvw.xy / v_uvw.z;
		vec4 t = texture2D (u_texture, uv);
		if (u_tex.w > 0.5 && (uv.x < 0.0 || uv.x >= 1.0 || uv.y < 0.0 || uv.y >= 1.0))
		{
			t = vec4 (0.0);
		}
		if (u_tex.z > 0.5 && t.a < 0.5)
		{
			discard;
		}
		base = mix (v_color.rgb, t.rgb, v_uvw.w);
	}
	vec3 c = base * min (v_light, 1.0) + (1.0 - base) * max (v_light - 1.0, 0.0) * (255.0 / 256.0);
	gl_FragColor = vec4 (c, v_color.a);
}

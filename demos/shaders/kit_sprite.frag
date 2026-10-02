// kit_sprite: the bitmap's texel (its holes are alpha 0) times the colour, or the colour alone
precision mediump float;

uniform sampler2D u_texture;
uniform vec4 u_mode;		// 1: textured; 1: holes are cut (not blended); 1: the texture is a shape (its alpha), the colour the vertices'; -

varying vec2 v_uv;
varying vec4 v_color;

void main ()
{
	vec4 c = v_color;
	if (u_mode.x > 0.5)
	{
		vec4 t = texture2D (u_texture, v_uv);
		if (u_mode.y > 0.5 && t.a < 0.5)
		{
			discard;
		}
		c = vec4 (u_mode.z > 0.5 ? v_color.rgb : t.rgb * v_color.rgb, t.a * v_color.a);
	}
	gl_FragColor = c;
}

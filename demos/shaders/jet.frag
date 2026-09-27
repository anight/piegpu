// jet: the colour, or the texel lit the way Jet lights it: a factor per
// channel, above 1 blowing out towards white (Renderer.cpp jetModulateRGB565)
precision mediump float;

uniform sampler2D u_texture;
uniform vec4 u_mode;		// textured, colour key, address mode (0 wrap, 1 clamp, 2 zero), -
uniform vec3 u_flat;		// texture LOD: the material colour the texture fades to
uniform float u_rows;		// > 0: scanlines (CRT): the scene's top row (GL y); odd rows only

varying vec2 v_uv;
varying vec4 v_color;
varying float v_texture;

void main ()
{
	vec4 c = v_color;
	if (u_rows > 0.0 && mod (floor (u_rows - gl_FragCoord.y), 2.0) < 0.5)
	{
		discard;
	}
	if (u_mode.x > 0.5)
	{
		vec2 uv = v_uv;
		bool outside = uv.x < 0.0 || uv.y < 0.0 || uv.x >= 1.0 || uv.y >= 1.0;
		uv = u_mode.z < 0.5 ? fract (uv) : clamp (uv, 0.0, 1.0);	// any size: GL wraps only 2^n
		vec4 t = texture2D (u_texture, uv);
		if (u_mode.y > 0.5 && t.a < 0.5)
		{
			discard;					// Jet's colour key
		}
		if (u_mode.z > 1.5 && outside)
		{
			t.rgb = vec3 (0.0);
		}
		vec3 base = mix (u_flat, t.rgb, v_texture);
		vec3 m = v_color.rgb * 2.0;
		c.rgb = base * min (m, 1.0) + (1.0 - base) * max (m - 1.0, 0.0);
	}
	gl_FragColor = c;
}

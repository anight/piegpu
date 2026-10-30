// stacker: the NES's screen, a texel a pixel holding a colour's number,
// coloured by the palette (16 x 1: the level's two colours, the field's
// background white in a four's flash). Shown at 4:3 as the NES was, scaled
// sharp: the four texels round the point coloured first, then blended only
// across a pixel's edge
precision mediump float;

uniform sampler2D u_tex;
uniform sampler2D u_pal;
uniform vec4 u_size;		// 1 / the texture's width, 1 / its height; screen pixels a game pixel, across, down

varying vec2 v_pixel;

vec3 colour (vec2 texel)
{
	float i = texture2D (u_tex, (texel + 0.5) * u_size.xy).r * 255.0;
	return texture2D (u_pal, vec2 ((i + 0.5) / 16.0, 0.5)).rgb;
}

void main ()
{
	vec2 p = v_pixel - 0.5;
	vec2 b = floor (p);
	vec2 f = clamp ((p - b - 0.5) * u_size.zw + 0.5, 0.0, 1.0);
	vec3 top = mix (colour (b), colour (b + vec2 (1.0, 0.0)), f.x);
	vec3 bottom = mix (colour (b + vec2 (0.0, 1.0)), colour (b + vec2 (1.0, 1.0)), f.x);
	gl_FragColor = vec4 (mix (top, bottom, f.y), 1.0);
}

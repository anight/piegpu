// hud: the font texture's alpha masks the vertex colour
precision mediump float;

uniform sampler2D u_font;

varying vec2 v_uv;
varying vec4 v_color;

void main ()
{
	gl_FragColor = vec4 (v_color.rgb, v_color.a * texture2D (u_font, v_uv).a);
}

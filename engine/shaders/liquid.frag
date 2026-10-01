// liquid: Quake's turbulence: each texel coordinate swayed by a sine of the
// other's (8 texels, a period of 16 pi texels), in time
precision mediump float;

uniform sampler2D u_texture;
uniform vec2 u_size;			// the texture's, texels
uniform float u_time;			// seconds

varying vec2 v_uv;

void main ()
{
	vec2 t = v_uv * u_size;
	vec2 uv = v_uv + 8.0 * sin (t.yx * 0.125 + u_time) / u_size;
	gl_FragColor = vec4 (texture2D (u_texture, uv).rgb, 1.0);
}

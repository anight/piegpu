// video.frag - the video texture as it is
precision mediump float;
uniform sampler2D u_video;
varying vec2 v_uv;

void main ()
{
	gl_FragColor = texture2D (u_video, v_uv);
}

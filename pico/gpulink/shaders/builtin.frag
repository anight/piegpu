precision mediump float;

uniform float u_mode;		// 0 FragCoord, 1 PointCoord, 2 FrontFacing, 3 DepthRange

void main ()
{
	if (u_mode < 0.5)
		gl_FragColor = vec4 (gl_FragCoord.x / 320.0, gl_FragCoord.y / 240.0, 0.0, 1.0);
	else if (u_mode < 1.5)
		gl_FragColor = vec4 (gl_PointCoord, 0.0, 1.0);
	else if (u_mode < 2.5)
		gl_FragColor = gl_FrontFacing ? vec4 (0.0, 1.0, 0.0, 1.0) : vec4 (1.0, 0.0, 0.0, 1.0);
	else
		gl_FragColor = vec4 (gl_DepthRange.near, gl_DepthRange.far, gl_DepthRange.diff, 1.0);
}

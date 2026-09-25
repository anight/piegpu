// control flow test (gltest.c): a loop with break, bounded by a uniform, and
// dynamic indexing of a uniform array
attribute vec2 a_pos;
uniform mat4 u_mvp;
uniform int u_n;
uniform int u_sel;
uniform vec4 u_k[8];
varying vec4 v_c;

void main ()
{
	vec4 c = vec4 (0.0);
	for (int i = 0; i < 8; i++)
	{
		if (i >= u_n)
			break;
		c.x += u_k[i].x;
	}
	c.yzw = u_k[u_sel].yzw;
	v_c = c;
	gl_Position = u_mvp * vec4 (a_pos, 0.0, 1.0);
}

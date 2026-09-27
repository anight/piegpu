/*
 * mat4.h - 4x4 matrices for the demos (column-major, as GL). The transform
 * functions multiply on the right: m = m * T, as the fixed-function GL did.
 */
#ifndef MAT4_H
#define MAT4_H

#include <math.h>
#include <string.h>

static inline void mat4_identity (float *m)
{
	memset (m, 0, 16 * sizeof (float));
	m[0] = m[5] = m[10] = m[15] = 1.0f;
}

static inline void mat4_multiply (float *r, const float *a, const float *b)
{
	float t[16];
	for (int c = 0; c < 4; c++)
		for (int i = 0; i < 4; i++)
			t[c*4 + i] =   a[0*4 + i] * b[c*4 + 0] + a[1*4 + i] * b[c*4 + 1]
				     + a[2*4 + i] * b[c*4 + 2] + a[3*4 + i] * b[c*4 + 3];
	memcpy (r, t, sizeof t);
}

static inline void mat4_translate (float *m, float x, float y, float z)
{
	float t[16];
	mat4_identity (t);
	t[12] = x;
	t[13] = y;
	t[14] = z;
	mat4_multiply (m, m, t);
}

static inline void mat4_scale (float *m, float x, float y, float z)
{
	float s[16];
	mat4_identity (s);
	s[0] = x;
	s[5] = y;
	s[10] = z;
	mat4_multiply (m, m, s);
}

/* rotate about axis 0 (x), 1 (y) or 2 (z), counter-clockwise */
static inline void mat4_rotate (float *m, int axis, float degrees)
{
	float r[16], s = sinf (degrees * 3.14159265f / 180.0f), c = cosf (degrees * 3.14159265f / 180.0f);
	int i = (axis + 1) % 3, j = (axis + 2) % 3;
	mat4_identity (r);
	r[i*4 + i] = c;
	r[i*4 + j] = s;
	r[j*4 + i] = -s;
	r[j*4 + j] = c;
	mat4_multiply (m, m, r);
}

static inline void mat4_perspective (float *m, float fovy_degrees, float aspect, float n, float f)
{
	float t = 1.0f / tanf (fovy_degrees * 3.14159265f / 360.0f);
	memset (m, 0, 16 * sizeof (float));
	m[0] = t / aspect;
	m[5] = t;
	m[10] = (f + n) / (n - f);
	m[11] = -1.0f;
	m[14] = 2*f*n / (n - f);
}

/* a view from eye towards center (gluLookAt) */
static inline void mat4_look_at (float *m, const float eye[3], const float center[3], const float up[3])
{
	float f[3] = {center[0] - eye[0], center[1] - eye[1], center[2] - eye[2]};
	float l = sqrtf (f[0]*f[0] + f[1]*f[1] + f[2]*f[2]);
	for (int i = 0; i < 3; i++)
		f[i] /= l;
	float s[3] = {f[1]*up[2] - f[2]*up[1], f[2]*up[0] - f[0]*up[2], f[0]*up[1] - f[1]*up[0]};
	l = sqrtf (s[0]*s[0] + s[1]*s[1] + s[2]*s[2]);
	for (int i = 0; i < 3; i++)
		s[i] /= l;
	float u[3] = {s[1]*f[2] - s[2]*f[1], s[2]*f[0] - s[0]*f[2], s[0]*f[1] - s[1]*f[0]};

	mat4_identity (m);
	for (int i = 0; i < 3; i++)
	{
		m[i*4 + 0] = s[i];
		m[i*4 + 1] = u[i];
		m[i*4 + 2] = -f[i];
	}
	m[12] = -(s[0]*eye[0] + s[1]*eye[1] + s[2]*eye[2]);
	m[13] = -(u[0]*eye[0] + u[1]*eye[1] + u[2]*eye[2]);
	m[14] = f[0]*eye[0] + f[1]*eye[1] + f[2]*eye[2];
}

#endif

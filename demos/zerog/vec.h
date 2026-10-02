/*
 * vec.h - zerog's vectors: three floats.
 */
#ifndef ZEROG_VEC_H
#define ZEROG_VEC_H

#include <math.h>
#include <string.h>

#define PI		3.14159265f

static inline void v_set (float *r, float x, float y, float z)	{ r[0] = x; r[1] = y; r[2] = z; }
static inline void v_copy (float *r, const float *a)			{ memcpy (r, a, 3 * sizeof (float)); }
static inline float v_dot (const float *a, const float *b)		{ return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
static inline float v_len (const float *a)				{ return sqrtf (v_dot (a, a)); }

static inline void v_sub (float *r, const float *a, const float *b)
{
	v_set (r, a[0] - b[0], a[1] - b[1], a[2] - b[2]);
}

static inline void v_cross (float *r, const float *a, const float *b)
{
	float t[3] = {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
	memcpy (r, t, sizeof t);
}

static inline void v_norm (float *v)
{
	float l = v_len (v);
	if (l > 0.0f)
	{
		v[0] /= l;
		v[1] /= l;
		v[2] /= l;
	}
}

/* r = a + b * s */
static inline void v_mad (float *r, const float *a, const float *b, float s)
{
	v_set (r, a[0] + b[0] * s, a[1] + b[1] * s, a[2] + b[2] * s);
}

/* r = a + (b - a) * w */
static inline void v_mix (float *r, const float *a, const float *b, float w)
{
	v_set (r, a[0] + (b[0] - a[0]) * w, a[1] + (b[1] - a[1]) * w, a[2] + (b[2] - a[2]) * w);
}

static inline float clampf (float x, float lo, float hi)	{ return x < lo ? lo : x > hi ? hi : x; }

static inline float smooth (float a, float b, float x)
{
	float t = clampf ((x - a) / (b - a), 0.0f, 1.0f);
	return t * t * (3.0f - 2.0f * t);
}

#endif

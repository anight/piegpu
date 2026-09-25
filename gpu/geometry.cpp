//
// geometry.cpp
//
#include "geometry.h"
#include "shaders.h"
#include <pgpu_protocol.h>
#include <circle/util.h>
#include <math.h>
#include <assert.h>

// V3D configuration bits (first three bytes of the CONFIGURATION_BITS record)
#define CFG_FORWARD		(1 << 0)
#define CFG_REVERSE		(1 << 1)
#define CFG_CLOCKWISE		(1 << 2)	// clockwise primitives are forward facing
#define CFG_DEPTH_FUNC__SHIFT	12
#define CFG_DEPTH_OFFSET	(1 << 3)
#define CFG_Z_UPDATE		(1 << 15)

// Guard band: clipped vertices stay this far outside the viewport at most.
// Measured on the hardware (320x240, a floor clipped against the guard band):
// up to 7x the half viewport (x -960 .. 1280 px) renders, 8x (x -1120 ..
// 1440 px) loses the triangles, although 12.4 fixed point goes to +-2048.
#define GUARD_PIXELS		800.0f
#define BATCH_VERTICES		3072		// multiple of 3
#define MAX_CLIP_VERTICES	12
#define CLIP_PLANES		6

static void Multiply (float *r, const float *a, const float *b)	// column-major 4x4
{
	for (unsigned c = 0; c < 4; c++)
		for (unsigned i = 0; i < 4; i++)
			r[c*4 + i] =   a[0*4 + i] * b[c*4 + 0] + a[1*4 + i] * b[c*4 + 1]
				     + a[2*4 + i] * b[c*4 + 2] + a[3*4 + i] * b[c*4 + 3];
}

static float Clamp01 (float f)
{
	return f < 0.0f ? 0.0f : f > 1.0f ? 1.0f : f;
}

static float Dot3 (const float *a, const float *b)
{
	return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

static void Normalize3 (float *v)
{
	float l = Dot3 (v, v);
	if (l > 0.0f)
	{
		l = 1.0f / sqrtf (l);
		v[0] *= l; v[1] *= l; v[2] *= l;
	}
}

static u32 FloatBits (float f)
{
	u32 u;
	memcpy (&u, &f, sizeof u);
	return u;
}

CGeometry::CGeometry (CRenderer *pRenderer, CTextures *pTextures)
:	m_pRenderer (pRenderer),
	m_pTextures (pTextures),
	m_nBatch (0)
{
	m_pVertices = new TVertex[MaxVertices];
	m_pBatch = new float[BATCH_VERTICES * (SCREEN_VERTEX_FIXED + MAX_VARYINGS)];
	memset (&m_Stats, 0, sizeof m_Stats);
}

CGeometry::~CGeometry (void)
{
}

TGeometryStats CGeometry::GetStats (void)
{
	TGeometryStats Stats = m_Stats;
	memset (&m_Stats, 0, sizeof m_Stats);
	return Stats;
}

u32 CGeometry::Draw (const TGLState &rState, u32 nMode, const TInputVertex *pVertices,
		     unsigned nVertices, const u32 *pIndices, unsigned nCount)
{
	if (nMode > PGPU_TRIANGLE_FAN)
	{
		return PGPU_ERR_ENUM;
	}
	if (nVertices > MaxVertices)
	{
		return PGPU_ERR_LIMIT;
	}

	m_pState = &rState;
	Multiply (m_MVP, rState.Projection, rState.Modelview);

	// normal matrix: inverse transpose of the modelview's upper 3x3
	const float *m = rState.Modelview;
	float a = m[0], b = m[4], c = m[8], d = m[1], e = m[5], f = m[9], g = m[2], h = m[6], k = m[10];
	float A = e * k - f * h, B = f * g - d * k, C = d * h - e * g;
	float fDet = a * A + b * B + c * C;
	float fInv = fDet != 0.0f ? 1.0f / fDet : 0.0f;
	// M^-T = cofactor matrix / det, row-major (N'_i = row i . N)
	m_NormalMatrix[0] = A * fInv;
	m_NormalMatrix[1] = B * fInv;
	m_NormalMatrix[2] = C * fInv;
	m_NormalMatrix[3] = (c * h - b * k) * fInv;
	m_NormalMatrix[4] = (a * k - c * g) * fInv;
	m_NormalMatrix[5] = (b * g - a * h) * fInv;
	m_NormalMatrix[6] = (b * f - c * e) * fInv;
	m_NormalMatrix[7] = (c * d - a * f) * fInv;
	m_NormalMatrix[8] = (a * e - b * d) * fInv;

	float fGuardX = rState.ViewportW > 0.0f ? 1.0f + GUARD_PIXELS / (rState.ViewportW * 0.5f) : 1.0f;
	float fGuardY = rState.ViewportH > 0.0f ? 1.0f + GUARD_PIXELS / (rState.ViewportH * 0.5f) : 1.0f;
	m_fGuard = fGuardX < fGuardY ? fGuardX : fGuardY;	// 6 for 320x240

	// transform the vertices that are used
	if (pIndices)
	{
		for (unsigned i = 0; i < nCount; i++)
		{
			if (pIndices[i] >= nVertices)
			{
				return PGPU_ERR_LIMIT;
			}
		}
	}
	for (unsigned i = 0; i < nVertices; i++)
	{
		Transform (rState, pVertices[i], &m_pVertices[i]);
	}

	boolean bFaces = nMode >= PGPU_TRIANGLES;
	SetupDraw (rState, bFaces);

	boolean bFlat = rState.nShadeModel == 1;
#define V(n)	(&m_pVertices[pIndices ? pIndices[n] : (n)])

	switch (nMode)
	{
	case PGPU_POINTS:
		for (unsigned i = 0; i < nCount; i++)
		{
			Point (V (i));
		}
		break;

	case PGPU_LINES:
		for (unsigned i = 0; i + 1 < nCount; i += 2)
		{
			Line (V (i), V (i + 1), bFlat ? 1 : 2);
		}
		break;

	case PGPU_LINE_STRIP:
	case PGPU_LINE_LOOP:
		for (unsigned i = 0; i + 1 < nCount; i++)
		{
			Line (V (i), V (i + 1), bFlat ? 1 : 2);
		}
		if (nMode == PGPU_LINE_LOOP && nCount > 2)
		{
			Line (V (nCount - 1), V (0), bFlat ? 1 : 2);
		}
		break;

	case PGPU_TRIANGLES:
		for (unsigned i = 0; i + 2 < nCount; i += 3)
		{
			Triangle (V (i), V (i + 1), V (i + 2), bFlat ? 2 : 3);
		}
		break;

	case PGPU_TRIANGLE_STRIP:		// keep the winding of every triangle
		for (unsigned i = 0; i + 2 < nCount; i++)
		{
			if (i & 1)
				Triangle (V (i + 1), V (i), V (i + 2), bFlat ? 2 : 3);
			else
				Triangle (V (i), V (i + 1), V (i + 2), bFlat ? 2 : 3);
		}
		break;

	case PGPU_TRIANGLE_FAN:
		for (unsigned i = 1; i + 1 < nCount; i++)
		{
			Triangle (V (0), V (i), V (i + 1), bFlat ? 2 : 3);
		}
		break;
	}
#undef V

	Flush ();

	return 0;
}

void CGeometry::Transform (const TGLState &rState, const TInputVertex &rIn, TVertex *pOut) const
{
	const float *p = rIn.Position;
	const float *mv = rState.Modelview;

	float Eye[4];
	for (unsigned i = 0; i < 4; i++)
	{
		Eye[i] = mv[i] * p[0] + mv[4 + i] * p[1] + mv[8 + i] * p[2] + mv[12 + i] * p[3];
	}
	for (unsigned i = 0; i < 4; i++)
	{
		pOut->Clip[i] =   m_MVP[i] * p[0] + m_MVP[4 + i] * p[1]
				+ m_MVP[8 + i] * p[2] + m_MVP[12 + i] * p[3];
	}

	if (rState.nEnables & PGPU_CAP_LIGHTING)
	{
		float N[3];
		for (unsigned i = 0; i < 3; i++)
		{
			N[i] = Dot3 (&m_NormalMatrix[i * 3], rIn.Normal);
		}
		if (rState.nEnables & PGPU_CAP_NORMALIZE)
		{
			Normalize3 (N);
		}

		float E[3] = {Eye[0] / Eye[3], Eye[1] / Eye[3], Eye[2] / Eye[3]};
		Light (rState, E, N, rIn.Color, pOut->Color[0]);
		if (rState.bTwoSide)
		{
			float NB[3] = {-N[0], -N[1], -N[2]};
			Light (rState, E, NB, rIn.Color, pOut->Color[1]);
		}
	}
	else
	{
		for (unsigned i = 0; i < 4; i++)
		{
			pOut->Color[0][i] = pOut->Color[1][i] = Clamp01 (rIn.Color[i]);
		}
	}

	// texture matrix * (s, t, 0, 1)
	const float *t = rState.Texture;
	float s = rIn.TexCoord[0], tt = rIn.TexCoord[1];
	float q = t[3] * s + t[7] * tt + t[15];
	float fInvQ = q != 0.0f ? 1.0f / q : 1.0f;
	pOut->TexCoord[0] = (t[0] * s + t[4] * tt + t[12]) * fInvQ;
	pOut->TexCoord[1] = (t[1] * s + t[5] * tt + t[13]) * fInvQ;

	if (rState.nEnables & PGPU_CAP_FOG)
	{
		float fDist = fabsf (Eye[2] / Eye[3]);	// eye plane distance
		float f;
		switch (rState.nFogMode)
		{
		case 0:
			f = rState.fFogEnd != rState.fFogStart
			    ? (rState.fFogEnd - fDist) / (rState.fFogEnd - rState.fFogStart) : 1.0f;
			break;
		case 1:
			f = expf (-rState.fFogDensity * fDist);
			break;
		default: {
			float x = rState.fFogDensity * fDist;
			f = expf (-x * x);
			} break;
		}
		pOut->fFog = Clamp01 (f);
	}
	else
	{
		pOut->fFog = 1.0f;
	}
}

// GL ES 1.1 section 2.12.1 (no spot lights, infinite viewer)
void CGeometry::Light (const TGLState &rState, const float *pEye, const float *pNormal,
		       const float *pVertexColor, float *pColor) const
{
	boolean bColorMaterial = !!(rState.nEnables & PGPU_CAP_COLOR_MATERIAL);
	const float *pAmbient = bColorMaterial ? pVertexColor : rState.MatAmbient;
	const float *pDiffuse = bColorMaterial ? pVertexColor : rState.MatDiffuse;

	float c[3];
	for (unsigned i = 0; i < 3; i++)
	{
		c[i] = rState.MatEmission[i] + pAmbient[i] * rState.LightModelAmbient[i];
	}

	for (unsigned n = 0; n < 4; n++)
	{
		if (!(rState.nEnables & (PGPU_CAP_LIGHT0 << n)))
		{
			continue;
		}

		const TLight &L = rState.Lights[n];
		float VP[3], fAtt = 1.0f;
		if (L.Position[3] == 0.0f)
		{
			VP[0] = L.Position[0]; VP[1] = L.Position[1]; VP[2] = L.Position[2];
		}
		else
		{
			float fInvW = 1.0f / L.Position[3];
			VP[0] = L.Position[0] * fInvW - pEye[0];
			VP[1] = L.Position[1] * fInvW - pEye[1];
			VP[2] = L.Position[2] * fInvW - pEye[2];
			float d = sqrtf (Dot3 (VP, VP));
			float fDen = L.Attenuation[0] + L.Attenuation[1] * d + L.Attenuation[2] * d * d;
			fAtt = fDen > 0.0f ? 1.0f / fDen : 1.0f;
		}
		Normalize3 (VP);

		float fNL = Dot3 (pNormal, VP);
		float fDiff = fNL > 0.0f ? fNL : 0.0f;
		float fSpec = 0.0f;
		if (fNL > 0.0f)
		{
			float H[3] = {VP[0], VP[1], VP[2] + 1.0f};
			Normalize3 (H);
			float fNH = Dot3 (pNormal, H);
			if (fNH > 0.0f)
			{
				fSpec = rState.fShininess > 0.0f ? powf (fNH, rState.fShininess) : 1.0f;
			}
		}

		for (unsigned i = 0; i < 3; i++)
		{
			c[i] += fAtt * (  pAmbient[i] * L.Ambient[i]
					+ fDiff * pDiffuse[i] * L.Diffuse[i]
					+ fSpec * rState.MatSpecular[i] * L.Specular[i]);
		}
	}

	pColor[0] = Clamp01 (c[0]);
	pColor[1] = Clamp01 (c[1]);
	pColor[2] = Clamp01 (c[2]);
	pColor[3] = Clamp01 (pDiffuse[3]);
}

// choose the fragment shader variant and fill its uniforms
void CGeometry::SetupDraw (const TGLState &rState, boolean bFaces)
{
	// an incomplete texture (or a cube map) disables texturing (GL ES 1.1)
	CTextures::TConfig Tex;
	memset (&Tex, 0, sizeof Tex);
	unsigned nTex = 0;
	if (   (rState.nEnables & PGPU_CAP_TEXTURE_2D)
	    && m_pTextures->Use (rState.nBoundTexture, &Tex) && !Tex.bCube)
	{
		nTex = rState.nTexEnvMode + 1;
		// ALPHA textures: RGB is Cf in every environment but DECAL (undefined);
		// MODULATE with RGB 1 gives that (A = Af At, exact but for REPLACE's At)
		if (Tex.bAlphaFormat)
		{
			nTex = PGPU_MODULATE + 1;
		}
	}
	u32 P0 = Tex.P0, P1 = Tex.P1;

	unsigned nFog = !!(rState.nEnables & PGPU_CAP_FOG);
	unsigned nAlpha = (rState.nEnables & PGPU_CAP_ALPHA_TEST) && rState.nAlphaFunc != PGPU_ALWAYS;
	unsigned nBlend = (rState.nEnables & PGPU_CAP_BLEND) || (rState.nColorMask & 0xF) != 0xF;

	unsigned nShader = nTex * 8 + nFog * 4 + nAlpha * 2 + nBlend;
	const TFragmentShader &Shader = FragmentShaders[nShader];
	m_nVaryings = Shader.nVaryings;

	// blend factor -> coefficients k0 + k1 As + k2 Ad + k3 Cs + k4 Cd
	float K[4][12];
	BlendCoefficients (rState, rState.bTargetAlpha, K);


	for (unsigned i = 0; i < Shader.nUniforms; i++)
	{
		unsigned nKind = Shader.pUniformKinds[i];
		u32 nValue = 0;

		switch (nKind)
		{
		case UNIFORM_TEX_P0:	nValue = P0; break;
		case UNIFORM_TEX_P1:	nValue = P1; break;
		case UNIFORM_TEX_RGB_MUL: nValue = FloatBits (Tex.bAlphaFormat ? 0.0f : 1.0f); break;
		case UNIFORM_TEX_RGB_ADD: nValue = FloatBits (Tex.bAlphaFormat ? 1.0f : 0.0f); break;
		case UNIFORM_ENV_R:	nValue = FloatBits (rState.TexEnvColor[0]); break;
		case UNIFORM_ENV_G:	nValue = FloatBits (rState.TexEnvColor[1]); break;
		case UNIFORM_ENV_B:	nValue = FloatBits (rState.TexEnvColor[2]); break;
		case UNIFORM_FOG_R:	nValue = FloatBits (rState.FogColor[0]); break;
		case UNIFORM_FOG_G:	nValue = FloatBits (rState.FogColor[1]); break;
		case UNIFORM_FOG_B:	nValue = FloatBits (rState.FogColor[2]); break;
		case UNIFORM_ALPHA_REF:	nValue = FloatBits (rState.fAlphaRef); break;

		case UNIFORM_STENCIL0:
		case UNIFORM_STENCIL1:
		case UNIFORM_STENCIL2: {
			u32 W[3];
			StencilWords (rState, W);
			nValue = W[nKind - UNIFORM_STENCIL0];
			} break;

		case UNIFORM_ALPHA_KG:	// pass if alpha > ref
		case UNIFORM_ALPHA_KL:	// pass if alpha < ref
		case UNIFORM_ALPHA_KE: {	// pass if alpha == ref
			static const u8 Pass[8] = {0, 2, 4, 6, 1, 3, 5, 7};	// bits: G, L, E
			u8 uchPass = Pass[rState.nAlphaFunc & 7];
			// NEVER 0, LESS L, EQUAL E, LEQUAL L|E, GREATER G, NOTEQUAL G|L, GEQUAL G|E, ALWAYS
			boolean bPass =   nKind == UNIFORM_ALPHA_KG ? (uchPass & 1)
					: nKind == UNIFORM_ALPHA_KL ? (uchPass & 2)
					: (uchPass & 4);
			nValue = FloatBits (bPass ? 1.0f : 0.0f);
			} break;

		default: {
			// fixed-function shaders: k0-k4 source, k5-k9 destination, terms
			// (1, As, Ad, Cs, Cd); SRC_ALPHA_SATURATE's term approximated by As
			assert (nKind >= UNIFORM_BLEND_R0);
			unsigned nIndex = nKind - UNIFORM_BLEND_R0;
			unsigned nChannel = nIndex / 10, k = nIndex % 10;
			const float *pSide = &K[nChannel][k < 5 ? 0 : 6];
			unsigned t = k % 5;
			float fCoeff = pSide[t] + (t == 1 ? pSide[5] : 0.0f);
			nValue = FloatBits (fCoeff);
			} break;
		}

		m_Uniforms[i] = nValue;
	}

	m_Setup.nShader = nShader;
	m_Setup.pUniforms = m_Uniforms;
	m_Setup.nUniforms = Shader.nUniforms;

	// configuration bits
	u32 nDepth;
	if (rState.nEnables & PGPU_CAP_DEPTH_TEST)
	{
		nDepth = rState.nDepthFunc << CFG_DEPTH_FUNC__SHIFT;
		if (rState.bDepthMask)
		{
			nDepth |= CFG_Z_UPDATE;
		}
	}
	else
	{
		nDepth = PGPU_ALWAYS << CFG_DEPTH_FUNC__SHIFT;	// no test, no depth writes
	}

	m_LineState.nConfigBits = CFG_FORWARD | CFG_REVERSE | nDepth;	// lines and points: no culling

	u32 nBits = CFG_FORWARD | CFG_REVERSE | ClockwiseBit (rState);
	if (rState.nEnables & PGPU_CAP_CULL_FACE)
	{
		if (rState.nCullFace == PGPU_BACK || rState.nCullFace == PGPU_FRONT_AND_BACK)
			nBits &= ~CFG_REVERSE;
		if (rState.nCullFace == PGPU_FRONT || rState.nCullFace == PGPU_FRONT_AND_BACK)
			nBits &= ~CFG_FORWARD;
	}
	GetDrawState (rState, FALSE, &m_LineState);
	m_TriangleState.nConfigBits = nBits | nDepth;
	GetDrawState (rState, TRUE, &m_TriangleState);

	m_bTwoSide = (rState.nEnables & PGPU_CAP_LIGHTING) && rState.bTwoSide && bFaces;
}

// signed distance to a clip plane: near, far, then the guard band (x, y)
float CGeometry::PlaneDistance (const TVertex &v, unsigned nPlane) const
{
	const float *c = v.Clip;
	switch (nPlane)
	{
	case 0:  return c[2] + c[3];			// near: z >= -w
	case 1:  return c[3] - c[2];			// far:  z <= w
	case 2:  return m_fGuard * c[3] - c[0];		// x <= G w
	case 3:  return m_fGuard * c[3] + c[0];		// x >= -G w
	case 4:  return m_fGuard * c[3] - c[1];
	default: return m_fGuard * c[3] + c[1];
	}
}

void CGeometry::Lerp (const TVertex &a, const TVertex &b, float t, TVertex *pOut)
{
	for (unsigned k = 0; k < 4; k++)
	{
		pOut->Clip[k] = a.Clip[k] + t * (b.Clip[k] - a.Clip[k]);
		pOut->Color[0][k] = a.Color[0][k] + t * (b.Color[0][k] - a.Color[0][k]);
		pOut->Color[1][k] = a.Color[1][k] + t * (b.Color[1][k] - a.Color[1][k]);
	}
	pOut->TexCoord[0] = a.TexCoord[0] + t * (b.TexCoord[0] - a.TexCoord[0]);
	pOut->TexCoord[1] = a.TexCoord[1] + t * (b.TexCoord[1] - a.TexCoord[1]);
	pOut->fFog = a.fFog + t * (b.fFog - a.fFog);
}

// Sutherland-Hodgman in clip space
unsigned CGeometry::ClipPolygon (TVertex *pIn, unsigned nIn, TVertex *pOut)
{
	TVertex Buffer[2][MAX_CLIP_VERTICES];
	TVertex *pSrc = pIn;
	unsigned nSrc = nIn;
	unsigned nBuffer = 0;

	for (unsigned nPlane = 0; nPlane < CLIP_PLANES && nSrc > 0; nPlane++)
	{
		TVertex *pDst = nPlane == CLIP_PLANES-1 ? pOut : Buffer[nBuffer];
		unsigned nDst = 0;

		for (unsigned i = 0; i < nSrc; i++)
		{
			const TVertex &a = pSrc[i];
			const TVertex &b = pSrc[(i + 1) % nSrc];
			float da = PlaneDistance (a, nPlane), db = PlaneDistance (b, nPlane);

			if (da >= 0.0f && nDst < MAX_CLIP_VERTICES)
			{
				pDst[nDst++] = a;
			}
			if ((da >= 0.0f) != (db >= 0.0f) && nDst < MAX_CLIP_VERTICES)
			{
				Lerp (a, b, da / (da - db), &pDst[nDst++]);
			}
		}

		pSrc = pDst;
		nSrc = nDst;
		nBuffer ^= 1;
	}

	return nSrc;
}

// parametric (Liang-Barsky style)
boolean CGeometry::ClipLine (const TVertex &a, const TVertex &b, TVertex *pOut)
{
	float t0 = 0.0f, t1 = 1.0f;
	for (unsigned nPlane = 0; nPlane < CLIP_PLANES; nPlane++)
	{
		float da = PlaneDistance (a, nPlane), db = PlaneDistance (b, nPlane);
		if (da < 0.0f && db < 0.0f)
		{
			return FALSE;
		}
		if (da < 0.0f)
		{
			float t = da / (da - db);
			if (t > t0) t0 = t;
		}
		else if (db < 0.0f)
		{
			float t = da / (da - db);
			if (t < t1) t1 = t;
		}
	}
	if (t0 > t1)
	{
		return FALSE;
	}

	Lerp (a, b, t0, &pOut[0]);
	Lerp (a, b, t1, &pOut[1]);

	return TRUE;
}

void CGeometry::Emit (const TVertex &v, unsigned nSide, const float *pColorOverride, float *pOut) const
{
	const TGLState &S = *m_pState;
	float fInvW = 1.0f / v.Clip[3];

	pOut[0] = S.ViewportX + (v.Clip[0] * fInvW + 1.0f) * 0.5f * S.ViewportW;
	// GL window origin is bottom-left, the panel's top-left
	float y = S.ViewportY + (v.Clip[1] * fInvW + 1.0f) * 0.5f * S.ViewportH;
	pOut[1] = S.bFlipY ? S.nTargetHeight - y : y;	// the panel's rows go top down
	pOut[2] = Clamp01 (S.DepthNear + (v.Clip[2] * fInvW + 1.0f) * 0.5f * (S.DepthFar - S.DepthNear));
	pOut[3] = fInvW;

	const float *c = pColorOverride ? pColorOverride : v.Color[nSide];
	float *p = pOut + SCREEN_VERTEX_FIXED;
	p[0] = c[0]; p[1] = c[1]; p[2] = c[2]; p[3] = c[3];
	unsigned n = 4;
	if (m_nVaryings == 6 || m_nVaryings == 7)
	{
		p[n++] = v.TexCoord[0];
		p[n++] = v.TexCoord[1];
	}
	if (m_nVaryings == 5 || m_nVaryings == 7)
	{
		p[n++] = v.fFog;
	}
}

void CGeometry::Triangle (const TVertex *a, const TVertex *b, const TVertex *c, unsigned nFlatColor)
{
	m_Stats.nPrimitives++;

	TVertex Poly[3] = {*a, *b, *c};

	// facing for two-sided lighting: orientation of the triangle in window space
	unsigned nSide = 0;
	if (m_bTwoSide)
	{
		const float *p = a->Clip, *q = b->Clip, *r = c->Clip;
		float fDet =   p[0] * (q[1] * r[3] - q[3] * r[1])
			     - p[1] * (q[0] * r[3] - q[3] * r[0])
			     + p[3] * (q[0] * r[1] - q[1] * r[0]);
		boolean bCCW = fDet > 0.0f;
		boolean bFront = m_pState->nFrontFace == PGPU_CCW ? bCCW : !bCCW;
		nSide = bFront ? 0 : 1;
	}

	// flat shading: the provoking vertex is the last one (GL)
	float FlatColor[4];
	const float *pFlat = nullptr;
	if (nFlatColor < 3)
	{
		memcpy (FlatColor, Poly[nFlatColor].Color[nSide], sizeof FlatColor);
		pFlat = FlatColor;
	}

	TVertex Clipped[MAX_CLIP_VERTICES];
	const TVertex *pPoly = Poly;
	unsigned nPoly = 3;

	boolean bInside = TRUE;
	for (unsigned i = 0; i < 3 && bInside; i++)
	{
		const float *v = Poly[i].Clip;
		float g = m_fGuard * v[3];
		if (!(v[2] >= -v[3] && v[2] <= v[3] && v[0] <= g && v[0] >= -g && v[1] <= g && v[1] >= -g))
		{
			bInside = FALSE;
		}
	}
	if (!bInside)
	{
		nPoly = ClipPolygon (Poly, 3, Clipped);
		pPoly = Clipped;
		if (nPoly < 3)
		{
			m_Stats.nRejected++;
			return;
		}
		m_Stats.nClipped++;
	}

	unsigned nFloats = SCREEN_VERTEX_FIXED + m_nVaryings;
	for (unsigned i = 1; i + 1 < nPoly; i++)
	{
		if (m_nBatch + 3 > BATCH_VERTICES || (m_nBatch && m_pBatchState != &m_TriangleState))
		{
			Flush ();
		}
		m_pBatchState = &m_TriangleState;

		Emit (pPoly[0],     nSide, pFlat, m_pBatch + (m_nBatch++) * nFloats);
		Emit (pPoly[i],     nSide, pFlat, m_pBatch + (m_nBatch++) * nFloats);
		Emit (pPoly[i + 1], nSide, pFlat, m_pBatch + (m_nBatch++) * nFloats);
	}
}

// screen-space quad (two triangles) with the attributes of a and b
void CGeometry::EmitQuad (const float Corner[4][2], const TVertex &a, const TVertex &b, unsigned nFlatColor)
{
	if (m_nBatch + 6 > BATCH_VERTICES || (m_nBatch && m_pBatchState != &m_LineState))
	{
		Flush ();
	}
	m_pBatchState = &m_LineState;

	const float *pFlat = nFlatColor < 2 ? (nFlatColor == 0 ? a.Color[0] : b.Color[0]) : nullptr;

	unsigned nFloats = SCREEN_VERTEX_FIXED + m_nVaryings;
	float Va[SCREEN_VERTEX_FIXED + MAX_VARYINGS], Vb[SCREEN_VERTEX_FIXED + MAX_VARYINGS];
	Emit (a, 0, pFlat, Va);
	Emit (b, 0, pFlat, Vb);

	// corners 0, 1 at a; 2, 3 at b
	static const unsigned Order[6] = {0, 1, 2, 0, 2, 3};
	for (unsigned i = 0; i < 6; i++)
	{
		unsigned n = Order[i];
		float *p = m_pBatch + (m_nBatch++) * nFloats;
		memcpy (p, n < 2 ? Va : Vb, nFloats * sizeof (float));
		p[0] = Corner[n][0];
		p[1] = Corner[n][1];
	}
}

void CGeometry::Line (const TVertex *a, const TVertex *b, unsigned nFlatColor)
{
	m_Stats.nPrimitives++;

	TVertex Clipped[2];
	if (!ClipLine (*a, *b, Clipped))
	{
		m_Stats.nRejected++;
		return;
	}

	const TVertex &p = Clipped[0], &q = Clipped[1];
	float Pa[SCREEN_VERTEX_FIXED + MAX_VARYINGS], Pb[SCREEN_VERTEX_FIXED + MAX_VARYINGS];
	Emit (p, 0, nullptr, Pa);
	Emit (q, 0, nullptr, Pb);

	float dx = Pb[0] - Pa[0], dy = Pb[1] - Pa[1];
	float l = sqrtf (dx * dx + dy * dy);
	if (l < 1e-3f)
	{
		dx = 1.0f; dy = 0.0f; l = 1.0f;
	}
	float fHalf = m_pState->fLineWidth * 0.5f;
	float nx = -dy / l * fHalf, ny = dx / l * fHalf;

	const float Corner[4][2] =
	{
		{Pa[0] + nx, Pa[1] + ny}, {Pa[0] - nx, Pa[1] - ny},
		{Pb[0] - nx, Pb[1] - ny}, {Pb[0] + nx, Pb[1] + ny},
	};
	EmitQuad (Corner, p, q, nFlatColor);
}

void CGeometry::Point (const TVertex *a)
{
	m_Stats.nPrimitives++;

	const float *c = a->Clip;
	if (!(c[3] > 0.0f && c[2] >= -c[3] && c[2] <= c[3]
	      && c[0] >= -c[3] && c[0] <= c[3] && c[1] >= -c[3] && c[1] <= c[3]))
	{
		m_Stats.nRejected++;
		return;
	}

	float P[SCREEN_VERTEX_FIXED + MAX_VARYINGS];
	Emit (*a, 0, nullptr, P);

	const float Corner[4][2] =		// 1 x 1 pixel
	{
		{P[0] - 0.5f, P[1] - 0.5f}, {P[0] - 0.5f, P[1] + 0.5f},
		{P[0] + 0.5f, P[1] + 0.5f}, {P[0] + 0.5f, P[1] - 0.5f},
	};
	EmitQuad (Corner, *a, *a, 2);
}

void CGeometry::BlendCoefficients (const TGLState &S, boolean bDstAlpha, float K[4][12])
{
	enum { T1, TAS, TAD, TSC, TDC, TSAT };
	memset (K, 0, sizeof (float) * 4 * 12);

	for (unsigned c = 0; c < 4; c++)
	{
		float *Fs = &K[c][0], *Fd = &K[c][6];

		if (!(S.nColorMask & (1 << c)))
		{
			Fd[T1] = 1.0f;			// keep the destination
			continue;
		}
		if (!(S.nEnables & PGPU_CAP_BLEND))
		{
			Fs[T1] = 1.0f;
			continue;
		}

		u32 nFactor[2] = {c < 3 ? S.nBlendSrc : S.nBlendSrcA, c < 3 ? S.nBlendDst : S.nBlendDstA};
		u32 nEquation = c < 3 ? S.nBlendEqRGB : S.nBlendEqA;
		float fSign[2] =
		{
			nEquation == PGPU_FUNC_REVERSE_SUBTRACT ? -1.0f : 1.0f,
			nEquation == PGPU_FUNC_SUBTRACT ? -1.0f : 1.0f
		};

		for (unsigned side = 0; side < 2; side++)
		{
			float *F = side ? Fd : Fs;
			switch (nFactor[side])
			{
			case PGPU_ZERO:					break;
			case PGPU_ONE:			F[T1] = 1;	break;
			case PGPU_SRC_COLOR:		F[TSC] = 1;	break;
			case PGPU_ONE_MINUS_SRC_COLOR:	F[T1] = 1; F[TSC] = -1;	break;
			case PGPU_SRC_ALPHA:		F[TAS] = 1;	break;
			case PGPU_ONE_MINUS_SRC_ALPHA:	F[T1] = 1; F[TAS] = -1;	break;
			case PGPU_DST_ALPHA:		F[TAD] = 1;	break;
			case PGPU_ONE_MINUS_DST_ALPHA:	F[T1] = 1; F[TAD] = -1;	break;
			case PGPU_DST_COLOR:		F[TDC] = 1;	break;
			case PGPU_ONE_MINUS_DST_COLOR:	F[T1] = 1; F[TDC] = -1;	break;
			case PGPU_SRC_ALPHA_SATURATE:	if (c < 3) F[TSAT] = 1; else F[T1] = 1;	break;
			case PGPU_CONSTANT_COLOR:	F[T1] = S.BlendColor[c];	break;
			case PGPU_ONE_MINUS_CONSTANT_COLOR: F[T1] = 1.0f - S.BlendColor[c]; break;
			case PGPU_CONSTANT_ALPHA:	F[T1] = S.BlendColor[3];	break;
			case PGPU_ONE_MINUS_CONSTANT_ALPHA: F[T1] = 1.0f - S.BlendColor[3]; break;
			}

			if (!bDstAlpha)				// destination alpha is 1
			{
				F[T1] += F[TAD];
				F[TAD] = 0.0f;
				F[TSAT] = 0.0f;			// min (As, 1 - 1) = 0
			}

			for (unsigned t = 0; t < 6; t++)
			{
				F[t] *= fSign[side];
			}
		}
	}
}

// TLB stencil setup words, as Mesa's vc4 (vc4_state.c): face (31-30),
// write mask code (29-28), zfail (27-25), zpass (24-22) and fail (21-19)
// ops, function (18-16), reference (15-8), value mask (7-0); the third word
// sets both full write masks
void CGeometry::StencilWords (const TGLState &S, u32 W[3])
{
	// pgpu_stencil_op -> V3D: ZERO 0, KEEP 1, REPLACE 2, INCR 3, DECR 4,
	// INVERT 5, INCR_WRAP 6, DECR_WRAP 7
	static const u8 OpMap[8] = {1, 0, 2, 3, 4, 5, 6, 7};
	boolean bTest = !!(S.nEnables & PGPU_CAP_STENCIL_TEST);

	for (unsigned f = 0; f < 2; f++)
	{
		u32 nWord;
		if (bTest)
		{
			nWord =   OpMap[S.StencilZFail[f] & 7] << 25
				| OpMap[S.StencilZPass[f] & 7] << 22
				| OpMap[S.StencilFail[f] & 7] << 19
				| (S.StencilFunc[f] & 7) << 16
				| (S.StencilRef[f] & 0xFF) << 8
				| (S.StencilValueMask[f] & 0xFF);
		}
		else
		{
			nWord = 1 << 25 | 1 << 22 | 1 << 19 | PGPU_ALWAYS << 16 | 0xFF;
		}
		W[f] = nWord | (f == 0 ? 1U : 2U) << 30;
	}

	// the full write masks (the mask codes in the first words aren't used)
	W[2] = bTest ? (S.StencilWriteMask[0] & 0xFF) | (S.StencilWriteMask[1] & 0xFF) << 8 : 0;
}

u32 CGeometry::ClockwiseBit (const TGLState &S)
{
	// with the panel's rows going top down, GL's CCW appears clockwise
	return (S.nFrontFace == PGPU_CCW) == !!S.bFlipY ? CFG_CLOCKWISE : 0;
}

void CGeometry::GetDrawState (const TGLState &S, boolean bFaces, TDrawState *pState)
{
	s32 nHeight = S.nTargetHeight;
	s32 x0 = 0, y0 = 0, x1 = S.nTargetWidth, y1 = nHeight;
	if (S.nEnables & PGPU_CAP_SCISSOR_TEST)
	{
		// GL window coordinates to target rows
		s32 sx0 = S.ScissorX, sx1 = S.ScissorX + (s32) S.ScissorW;
		s32 sy0 = S.ScissorY, sy1 = S.ScissorY + (s32) S.ScissorH;
		if (S.bFlipY)
		{
			sy0 = nHeight - (S.ScissorY + (s32) S.ScissorH);
			sy1 = nHeight - S.ScissorY;
		}
		if (sx0 > x0) x0 = sx0;
		if (sy0 > y0) y0 = sy0;
		if (sx1 < x1) x1 = sx1;
		if (sy1 < y1) y1 = sy1;
		if (x1 < x0) x1 = x0;
		if (y1 < y0) y1 = y0;
	}
	pState->nClipX = x0;
	pState->nClipY = y0;
	pState->nClipWidth = x1 - x0;
	pState->nClipHeight = y1 - y0;

	pState->nDepthOffset = 0;
	if (bFaces && (S.nEnables & PGPU_CAP_POLYGON_OFFSET_FILL))
	{
		// factor and units as float 1-8-7 (the top 16 bits of a float, as Mesa's vc4)
		u32 nFactor, nUnits;
		memcpy (&nFactor, &S.fOffsetFactor, 4);
		memcpy (&nUnits, &S.fOffsetUnits, 4);
		pState->nDepthOffset = (nFactor >> 16) | (nUnits >> 16) << 16;
		pState->nConfigBits |= CFG_DEPTH_OFFSET;
	}

	pState->fLineWidth = S.fLineWidth;
}

void CGeometry::Flush (void)
{
	if (m_nBatch)
	{
		m_Setup.State = *m_pBatchState;
		m_pRenderer->AddTriangles (m_Setup, m_pBatch, m_nBatch);
		m_nBatch = 0;
	}
}

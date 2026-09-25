//
// geometry.h
//
// The ARM side of the fixed-function pipeline (GL ES 1.1 semantics):
// vertex transformation, lighting, texture matrix, fog, primitive assembly,
// clipping, flat shading, lines and points as screen-space quads, and the
// choice of fragment shader variant and its uniforms. Emits screen-space
// triangles to the renderer.
//
#ifndef _geometry_h
#define _geometry_h

#include "renderer.h"
#include "textures.h"
#include <circle/types.h>

struct TLight
{
	float Position[4];		// eye space; w = 0: directional
	float Ambient[4], Diffuse[4], Specular[4];
	float Attenuation[3];		// constant, linear, quadratic
};

struct TGLState
{
	float Modelview[16], Projection[16], Texture[16];

	float ViewportX, ViewportY, ViewportW, ViewportH;
	float DepthNear, DepthFar;

	u32 nEnables;
	u32 nDepthFunc;
	boolean bDepthMask;
	u32 nBlendSrc, nBlendDst;
	u32 nCullFace, nFrontFace;
	u32 nAlphaFunc;
	float fAlphaRef;
	u32 nColorMask;			// bits 0-3: R, G, B, A
	u32 nShadeModel;		// 0 smooth, 1 flat

	float Color[4], Normal[3], TexCoord[2];

	TLight Lights[4];
	float MatAmbient[4], MatDiffuse[4], MatSpecular[4], MatEmission[4];
	float fShininess;
	float LightModelAmbient[4];
	boolean bTwoSide;

	u32 nFogMode;			// 0 linear, 1 exp, 2 exp2
	float FogColor[4];
	float fFogStart, fFogEnd, fFogDensity;

	u32 nTexEnvMode;		// 0 modulate, 1 replace, 2 decal, 3 blend
	float TexEnvColor[4];
	u32 nBoundTexture;
};

// a vertex as fetched from arrays or DRAW_INLINE
struct TInputVertex
{
	float Position[4];
	float Color[4];
	float Normal[3];
	float TexCoord[2];
};

struct TGeometryStats
{
	unsigned nPrimitives;
	unsigned nClipped;		// primitives that needed clipping
	unsigned nRejected;		// entirely outside
};

class CGeometry
{
public:
	static const unsigned MaxVertices = 65536;	// per draw call

public:
	CGeometry (CRenderer *pRenderer, CTextures *pTextures);
	~CGeometry (void);

	/// \brief Draw primitives from processed input vertices
	/// \param pIndices nullptr: vertices 0 .. nCount-1 in order
	/// \return 0 or a pgpu_error code
	u32 Draw (const TGLState &rState, u32 nMode, const TInputVertex *pVertices,
		  unsigned nVertices, const u32 *pIndices, unsigned nCount);

	TGeometryStats GetStats (void);

private:
	// a transformed vertex
	struct TVertex
	{
		float Clip[4];
		float Color[2][4];	// front, back (two-sided lighting)
		float TexCoord[2];
		float fFog;
	};

	void Transform (const TGLState &rState, const TInputVertex &rIn, TVertex *pOut) const;
	void Light (const TGLState &rState, const float *pEye, const float *pNormal,
		    const float *pVertexColor, float *pColor) const;

	void SetupDraw (const TGLState &rState, boolean bFaces);
	void Triangle (const TVertex *a, const TVertex *b, const TVertex *c, unsigned nFlatColor);
	void Line (const TVertex *a, const TVertex *b, unsigned nFlatColor);
	void Point (const TVertex *a);

	float PlaneDistance (const TVertex &v, unsigned nPlane) const;
	static void Lerp (const TVertex &a, const TVertex &b, float t, TVertex *pOut);
	unsigned ClipPolygon (TVertex *pIn, unsigned nIn, TVertex *pOut);
	boolean ClipLine (const TVertex &a, const TVertex &b, TVertex *pOut);
	void Emit (const TVertex &v, unsigned nSide, const float *pColorOverride, float *pOut) const;
	void EmitQuad (const float Corner[4][2], const TVertex &a, const TVertex &b, unsigned nFlatColor);
	void Flush (void);

private:
	CRenderer *m_pRenderer;
	CTextures *m_pTextures;

	TVertex *m_pVertices;

	// current draw
	const TGLState *m_pState;
	float m_MVP[16];
	float m_NormalMatrix[9];
	float m_fGuard;			// guard band in NDC units
	TDrawSetup m_Setup;
	u32 m_Uniforms[64];
	unsigned m_nVaryings;
	u32 m_nTriangleConfig, m_nLineConfig;
	boolean m_bTwoSide;

	float *m_pBatch;		// screen vertices waiting for AddTriangles
	unsigned m_nBatch;
	u32 m_nBatchConfig;

	TGeometryStats m_Stats;
};

#endif

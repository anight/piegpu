//
// scene.cpp
//
#include "scene.h"
#include "shaders.h"
#include <circle/logger.h>
#include <circle/util.h>
#include <math.h>
#include <assert.h>

LOGMODULE ("scene");

#define BIN_CL_SIZE		4096
#define RENDER_CL_SIZE		(64 * 1024)
#define TILE_ALLOC_SIZE		(4 * 1024 * 1024)
#define OVERFLOW_SIZE		(1 * 1024 * 1024)
#define MAX_TILES		(32 * 32)

#define CUBE_VERTICES		36
#define CUBE_STRIDE		24	// Xs, Ys (12.4), Zs, 1/Wc, r, g, b
#define PLASMA_VERTICES		6
#define PLASMA_STRIDE		12	// Xs, Ys (12.4), Zs, 1/Wc

static const float TurnsPerRadian = 0.15915494f;	// 1 / (2 pi)

CScene::CScene (CV3D *pV3D)
:	m_pV3D (pV3D),
	m_nWidth (0),
	m_nHeight (0)
{
}

CScene::~CScene (void)
{
}

boolean CScene::Initialize (unsigned nMaxWidth, unsigned nMaxHeight)
{
	m_pBinCL = (u8 *) CV3D::Alloc (BIN_CL_SIZE);
	for (unsigned i = 0; i < FrameBuffers; i++)
	{
		m_pRenderCL[i] = (u8 *) CV3D::Alloc (RENDER_CL_SIZE);
	}
	m_pShaderRecords = (u8 *) CV3D::Alloc (64);
	m_pCubeCode = (u32 *) CV3D::Alloc (sizeof CubeShader);
	m_pPlasmaCode = (u32 *) CV3D::Alloc (sizeof PlasmaShader);
	m_pUniforms = (float *) CV3D::Alloc (PlasmaShader_Count * sizeof (float));
	m_pPlasmaVertices = (u8 *) CV3D::Alloc (PLASMA_VERTICES * PLASMA_STRIDE);
	m_pCubeVertices = (u8 *) CV3D::Alloc (CUBE_VERTICES * CUBE_STRIDE);
	m_pTileAlloc = (u8 *) CV3D::Alloc (TILE_ALLOC_SIZE);
	m_pTileState = (u8 *) CV3D::Alloc (MAX_TILES * V3D_TILE_STATE_SIZE);
	m_pOverflow = (u8 *) CV3D::Alloc (OVERFLOW_SIZE);
	m_nFrameBufferSize = nMaxWidth * nMaxHeight * sizeof (u32);
	for (unsigned i = 0; i < FrameBuffers; i++)
	{
		m_pFrameBuffer[i] = CV3D::Alloc (m_nFrameBufferSize);
	}

	memcpy (m_pCubeCode, CubeShader, sizeof CubeShader);
	memcpy (m_pPlasmaCode, PlasmaShader, sizeof PlasmaShader);
	CV3D::Flush (m_pCubeCode, sizeof CubeShader);
	CV3D::Flush (m_pPlasmaCode, sizeof PlasmaShader);

	LOGNOTE ("Shaders: cube %u, plasma %u instructions",
		 sizeof CubeShader / 8, sizeof PlasmaShader / 8);

	return TRUE;
}

static void PutVertex (u8 *p, float x, float y, float z, float fInvW)
{
	CControlList v (p, 12);
	v.Add16 ((u16) (s16) (x * 16.0f));	// 12.4 fixed point
	v.Add16 ((u16) (s16) (y * 16.0f));
	v.AddFloat (z);
	v.AddFloat (fInvW);
}

boolean CScene::SetResolution (unsigned nWidth, unsigned nHeight, boolean bRGB565)
{
	m_nWidth = nWidth;
	m_nHeight = nHeight;

	unsigned nTilesX = (nWidth + V3D_TILE_SIZE-1) / V3D_TILE_SIZE;
	unsigned nTilesY = (nHeight + V3D_TILE_SIZE-1) / V3D_TILE_SIZE;
	assert (nTilesX * nTilesY <= MAX_TILES);

	// plasma: two triangles covering the screen, behind the cube
	const float Z = 0.999f;
	PutVertex (m_pPlasmaVertices + 0*PLASMA_STRIDE, 0, 0, Z, 1);
	PutVertex (m_pPlasmaVertices + 1*PLASMA_STRIDE, nWidth, 0, Z, 1);
	PutVertex (m_pPlasmaVertices + 2*PLASMA_STRIDE, 0, nHeight, Z, 1);
	PutVertex (m_pPlasmaVertices + 3*PLASMA_STRIDE, nWidth, 0, Z, 1);
	PutVertex (m_pPlasmaVertices + 4*PLASMA_STRIDE, nWidth, nHeight, Z, 1);
	PutVertex (m_pPlasmaVertices + 5*PLASMA_STRIDE, 0, nHeight, Z, 1);
	CV3D::Flush (m_pPlasmaVertices, PLASMA_VERTICES * PLASMA_STRIDE);

	// NV shader state records (16 bytes each)
	CControlList Rec (m_pShaderRecords, 64);
	Rec.Add8 (0x01);			// fragment shader single threaded
	Rec.Add8 (PLASMA_STRIDE);
	Rec.Add8 (0);
	Rec.Add8 (0);				// varyings
	Rec.Add32 (CV3D::BusAddress (m_pPlasmaCode));
	Rec.Add32 (CV3D::BusAddress (m_pUniforms));
	Rec.Add32 (CV3D::BusAddress (m_pPlasmaVertices));

	Rec.Add8 (0x01);
	Rec.Add8 (CUBE_STRIDE);
	Rec.Add8 (0);
	Rec.Add8 (3);				// varyings: r, g, b
	Rec.Add32 (CV3D::BusAddress (m_pCubeCode));
	Rec.Add32 (CV3D::BusAddress (m_pUniforms));	// unused
	Rec.Add32 (CV3D::BusAddress (m_pCubeVertices));
	Rec.Flush ();

	// binning control list
	CControlList Bin (m_pBinCL, BIN_CL_SIZE);
	Bin.Add8 (V3D_TILE_BINNING_MODE_CONFIG);
	Bin.Add32 (CV3D::BusAddress (m_pTileAlloc));
	Bin.Add32 (TILE_ALLOC_SIZE);
	Bin.Add32 (CV3D::BusAddress (m_pTileState));
	Bin.Add8 (nTilesX);
	Bin.Add8 (nTilesY);
	Bin.Add8 (0x04);			// auto-initialise tile state data array

	Bin.Add8 (V3D_START_TILE_BINNING);

	Bin.Add8 (V3D_CLIP_WINDOW);
	Bin.Add16 (0);
	Bin.Add16 (0);
	Bin.Add16 (nWidth);
	Bin.Add16 (nHeight);

	Bin.Add8 (V3D_CONFIGURATION_BITS);
	Bin.Add8 (0x03);			// forward and reverse facing primitives
	Bin.Add8 (0x90);			// depth test LESS (bits 12-14), Z updates (bit 15)
	Bin.Add8 (0x00);

	Bin.Add8 (V3D_VIEWPORT_OFFSET);
	Bin.Add16 (0);
	Bin.Add16 (0);

	Bin.Add8 (V3D_NV_SHADER_STATE);
	Bin.Add32 (CV3D::BusAddress (m_pShaderRecords));
	Bin.Add8 (V3D_VERTEX_ARRAY_PRIMITIVES);
	Bin.Add8 (V3D_PRIM_TRIANGLES);
	Bin.Add32 (PLASMA_VERTICES);
	Bin.Add32 (0);

	Bin.Add8 (V3D_NV_SHADER_STATE);
	Bin.Add32 (CV3D::BusAddress (m_pShaderRecords + 16));
	Bin.Add8 (V3D_VERTEX_ARRAY_PRIMITIVES);
	Bin.Add8 (V3D_PRIM_TRIANGLES);
	Bin.Add32 (CUBE_VERTICES);
	Bin.Add32 (0);

	Bin.Add8 (V3D_FLUSH);
	Bin.Add8 (V3D_NOP);
	assert (!Bin.Overflow ());
	Bin.Flush ();
	m_nBinStart = Bin.GetStartBus ();
	m_nBinEnd = Bin.GetEndBus ();

	// rendering control lists, one per frame buffer
	size_t nRenderSize = 0;
	for (unsigned nBuffer = 0; nBuffer < FrameBuffers; nBuffer++)
	{
		CControlList Render (m_pRenderCL[nBuffer], RENDER_CL_SIZE);
		Render.Add8 (V3D_CLEAR_COLORS);
		Render.Add32 (0xFF000000);
		Render.Add32 (0xFF000000);
		Render.Add16 (0xFFFF);			// clear Z 0xFFFFFF (24 bits) ...
		Render.Add8 (0xFF);
		Render.Add8 (0);			// ... clear VG mask
		Render.Add8 (0);			// clear stencil

		Render.Add8 (V3D_TILE_RENDERING_MODE_CONFIG);
		Render.Add32 (CV3D::BusAddress (m_pFrameBuffer[nBuffer]));
		Render.Add16 (nWidth);
		Render.Add16 (nHeight);
		// frame buffer format (bits 2-3): 0 = BGR565 dithered, 1 = RGBA8888; linear
		Render.Add16 (bRGB565 ? 0 << 2 : 1 << 2);

		// dummy store to clear the tile buffer
		Render.Add8 (V3D_TILE_COORDINATES);
		Render.Add8 (0);
		Render.Add8 (0);
		Render.Add8 (V3D_STORE_TILE_BUFFER_GENERAL);
		Render.Add16 (0);
		Render.Add32 (0);

		for (unsigned y = 0; y < nTilesY; y++)
		{
			for (unsigned x = 0; x < nTilesX; x++)
			{
				Render.Add8 (V3D_TILE_COORDINATES);
				Render.Add8 (x);
				Render.Add8 (y);

				Render.Add8 (V3D_BRANCH_TO_SUBLIST);
				Render.Add32 (CV3D::BusAddress (m_pTileAlloc)
					      + (y * nTilesX + x) * V3D_TILE_ALLOC_BLOCK);

				boolean bLast = x == nTilesX-1 && y == nTilesY-1;
				Render.Add8 (bLast ? V3D_STORE_MS_TILE_BUFFER_EOF : V3D_STORE_MS_TILE_BUFFER);
			}
		}
		assert (!Render.Overflow ());
		Render.Flush ();
		m_nRenderStart[nBuffer] = Render.GetStartBus ();
		m_nRenderEnd[nBuffer] = Render.GetEndBus ();
		nRenderSize = Render.GetSize ();
	}

	LOGNOTE ("%ux%u %s: %ux%u tiles, bin CL %u bytes, render CL %u bytes",
		 nWidth, nHeight, bRGB565 ? "RGB565" : "BGRA8888", nTilesX, nTilesY,
		 Bin.GetSize (), nRenderSize);

	return TRUE;
}

boolean CScene::Render (float fTime, unsigned nBuffer, unsigned *pBinUs, unsigned *pRenderUs)
{
	assert (nBuffer < FrameBuffers);

	UpdatePlasmaUniforms (fTime);
	UpdateCubeVertices (fTime);

	return m_pV3D->RunJob (m_nBinStart, m_nBinEnd, m_nRenderStart[nBuffer], m_nRenderEnd[nBuffer],
			       CV3D::BusAddress (m_pOverflow), OVERFLOW_SIZE,
			       pBinUs, pRenderUs);
}

void CScene::UpdatePlasmaUniforms (float fTime)
{
	// GL scene: p = 3 * (ndc.x * aspect, ndc.y); here q = p / 2pi (turns)
	float fAspect = (float) m_nWidth / m_nHeight;
	float k = TurnsPerRadian;
	float *u = m_pUniforms;

	// pixel centre (x + 0.5) -> ndc; y points down on screen
	u[PlasmaShader_ax] = 2.0f / m_nWidth * fAspect * 3.0f * k;
	u[PlasmaShader_bx] = (1.0f / m_nWidth - 1.0f) * fAspect * 3.0f * k;
	u[PlasmaShader_ay] = -2.0f / m_nHeight * 3.0f * k;
	u[PlasmaShader_by] = (1.0f - 1.0f / m_nHeight) * 3.0f * k;

	const float Offset = 512.5f;		// keeps arguments positive, centres the fraction
	u[PlasmaShader_t1] = fTime * k + Offset;
	u[PlasmaShader_k13] = 1.3f;
	u[PlasmaShader_t2] = 0.7f * fTime * k + Offset;
	u[PlasmaShader_k08] = 0.8f;
	u[PlasmaShader_t3] = 1.3f * fTime * k + Offset;
	u[PlasmaShader_k15] = 1.5f;
	u[PlasmaShader_t4] = -fTime * k + Offset;

	// cos (K + 1.2 v) = sin (2 pi ((K + 1.2 v) / 2pi + 0.25))
	static const float K[3] = {0.0f, 2.1f, 4.2f};
	static const int M[3] = {PlasmaShader_m0, PlasmaShader_m1, PlasmaShader_m2};
	static const int D[3] = {PlasmaShader_d0, PlasmaShader_d1, PlasmaShader_d2};
	for (unsigned i = 0; i < 3; i++)
	{
		u[M[i]] = 1.2f * k;
		u[D[i]] = K[i] * k + 0.25f + Offset;
	}

	CV3D::Flush (m_pUniforms, PlasmaShader_Count * sizeof (float));
}

// Column-major 4x4 matrices (as in gles-demo)
static void MatMultiply (float *pResult, const float *a, const float *b)
{
	float r[16];
	for (unsigned c = 0; c < 4; c++)
	{
		for (unsigned row = 0; row < 4; row++)
		{
			r[c*4 + row] =   a[0*4 + row] * b[c*4 + 0] + a[1*4 + row] * b[c*4 + 1]
				       + a[2*4 + row] * b[c*4 + 2] + a[3*4 + row] * b[c*4 + 3];
		}
	}
	memcpy (pResult, r, sizeof r);
}

void CScene::UpdateCubeVertices (float fTime)
{
	static const struct
	{
		float n[3], u[3], v[3], c[3];
	}
	Faces[6] =
	{
		{{ 1, 0, 0}, {0, 1, 0}, {0, 0, 1}, {1.0f, 0.3f, 0.3f}},
		{{-1, 0, 0}, {0, 0, 1}, {0, 1, 0}, {0.3f, 1.0f, 0.3f}},
		{{ 0, 1, 0}, {0, 0, 1}, {1, 0, 0}, {0.3f, 0.4f, 1.0f}},
		{{ 0,-1, 0}, {1, 0, 0}, {0, 0, 1}, {1.0f, 1.0f, 0.3f}},
		{{ 0, 0, 1}, {1, 0, 0}, {0, 1, 0}, {0.3f, 1.0f, 1.0f}},
		{{ 0, 0,-1}, {0, 1, 0}, {1, 0, 0}, {1.0f, 0.3f, 1.0f}},
	};
	static const float Corners[6][2] = {{-1,-1}, {1,-1}, {1,1},  {-1,-1}, {1,1}, {-1,1}};

	// model: rotate X, then Y, move back; projection as in gles-demo
	float ax = fTime * 0.9f, ay = fTime * 1.3f;
	float sx = sinf (ax), cx = cosf (ax), sy = sinf (ay), cy = cosf (ay);
	float Rx[16] = {1, 0, 0, 0,  0, cx, sx, 0,  0, -sx, cx, 0,  0, 0, 0, 1};
	float Ry[16] = {cy, 0, -sy, 0,  0, 1, 0, 0,  sy, 0, cy, 0,  0, 0, 0, 1};
	float Model[16];
	MatMultiply (Model, Rx, Ry);
	Model[14] = -3.0f;

	float fAspect = (float) m_nWidth / m_nHeight;
	float f = 1.0f / tanf (0.9f / 2.0f), fNear = 0.5f, fFar = 10.0f;
	float Proj[16] = {0};
	Proj[0] = f / fAspect;
	Proj[5] = f;
	Proj[10] = (fFar + fNear) / (fNear - fFar);
	Proj[11] = -1.0f;
	Proj[14] = 2.0f * fFar * fNear / (fNear - fFar);

	float MVP[16];
	MatMultiply (MVP, Proj, Model);

	// light direction (normalized (0.4, 0.7, 1.0))
	const float L[3] = {0.3268f, 0.5718f, 0.8168f};

	u8 *p = m_pCubeVertices;
	for (unsigned face = 0; face < 6; face++)
	{
		// lighting per face normal (per vertex in GL, same value on a flat face)
		float n[3];
		for (unsigned r = 0; r < 3; r++)
		{
			n[r] = Model[0*4 + r] * Faces[face].n[0] + Model[1*4 + r] * Faces[face].n[1]
			     + Model[2*4 + r] * Faces[face].n[2];
		}
		float fDiffuse = n[0] * L[0] + n[1] * L[1] + n[2] * L[2];
		if (fDiffuse < 0.0f)
		{
			fDiffuse = 0.0f;
		}
		float fLight = 0.25f + 0.75f * fDiffuse;

		for (unsigned i = 0; i < 6; i++, p += CUBE_STRIDE)
		{
			float v[4];
			for (unsigned k = 0; k < 3; k++)
			{
				v[k] = 0.5f * (Faces[face].n[k] + Corners[i][0] * Faces[face].u[k]
								+ Corners[i][1] * Faces[face].v[k]);
			}
			v[3] = 1.0f;

			float c[4];
			for (unsigned r = 0; r < 4; r++)
			{
				c[r] =   MVP[0*4 + r] * v[0] + MVP[1*4 + r] * v[1]
				       + MVP[2*4 + r] * v[2] + MVP[3*4 + r] * v[3];
			}

			float fInvW = 1.0f / c[3];
			float xs = (c[0] * fInvW * 0.5f + 0.5f) * m_nWidth;
			float ys = (0.5f - c[1] * fInvW * 0.5f) * m_nHeight;	// y down
			float zs = c[2] * fInvW * 0.5f + 0.5f;

			PutVertex (p, xs, ys, zs, fInvW);

			CControlList Vary (p + 12, 12);
			Vary.AddFloat (Faces[face].c[0] * fLight);
			Vary.AddFloat (Faces[face].c[1] * fLight);
			Vary.AddFloat (Faces[face].c[2] * fLight);
		}
	}

	CV3D::Flush (m_pCubeVertices, CUBE_VERTICES * CUBE_STRIDE);
}

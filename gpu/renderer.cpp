//
// renderer.cpp
//
#include "renderer.h"
#include "shaders.h"
#include <circle/timer.h>
#include <circle/util.h>
#include <assert.h>

#define PIN_FRAME		26

#define BIN_CL_SIZE		(512 * 1024)
#define RENDER_CL_SIZE		(16 * 1024)
#define TILE_ALLOC_SIZE		(4 * 1024 * 1024)
#define OVERFLOW_SIZE		(1 * 1024 * 1024)
#define MAX_TILES		(32 * 32)

// Load/Store Tile Buffer General (as in Mesa's vc4_packet.h)
#define LOADSTORE_BUFFER_COLOR		1		// bits 0-2
#define LOADSTORE_TILING_RASTER		(0 << 4)
#define LOADSTORE_FORMAT_BGR565		(2 << 8)	// no dither

static unsigned NVStride (unsigned nShader)
{
	return 12 + 4 * FragmentShaders[nShader].nVaryings;	// Xs, Ys (12.4), Zs, 1/Wc, varyings
}

CRenderer::CRenderer (CV3D *pV3D, CST7789DMADisplay *pDisplay)
:	m_pV3D (pV3D),
	m_pDisplay (pDisplay),
	m_PinFrame (PIN_FRAME, GPIOModeOutput),
	m_nBuffer (0),
	m_nDraws (0),
	m_nVertexBytes (0),
	m_nUniformWords (0),
	m_nDropped (0)
{
	m_nRecordBytes = 0;
	m_PinFrame.Write (LOW);
}

CRenderer::~CRenderer (void)
{
}

unsigned CRenderer::GetVaryings (unsigned nShader)
{
	assert (nShader < FRAGMENT_SHADERS);
	return FragmentShaders[nShader].nVaryings;
}

boolean CRenderer::Initialize (void)
{
	m_nWidth = m_pDisplay->GetWidth ();
	m_nHeight = m_pDisplay->GetHeight ();
	m_nTilesX = (m_nWidth + V3D_TILE_SIZE-1) / V3D_TILE_SIZE;
	m_nTilesY = (m_nHeight + V3D_TILE_SIZE-1) / V3D_TILE_SIZE;
	assert (m_nTilesX * m_nTilesY <= MAX_TILES);

	m_pBinCL = (u8 *) CV3D::Alloc (BIN_CL_SIZE);
	m_pRenderCL = (u8 *) CV3D::Alloc (RENDER_CL_SIZE);
	m_pRecords = (u8 *) CV3D::Alloc (MaxDraws * 16);
	m_pRecordPool = (u8 *) CV3D::Alloc (RecordPoolBytes);
	m_pVertexPool = (u8 *) CV3D::Alloc (VertexPoolBytes);
	m_pUniformPool = (u32 *) CV3D::Alloc (UniformPoolWords * 4);
	m_pTileAlloc = (u8 *) CV3D::Alloc (TILE_ALLOC_SIZE);
	m_pTileState = (u8 *) CV3D::Alloc (MAX_TILES * V3D_TILE_STATE_SIZE);
	m_pOverflow = (u8 *) CV3D::Alloc (OVERFLOW_SIZE);
	for (unsigned i = 0; i < 2; i++)
	{
		m_pFrameBuffer[i] = (u16 *) CV3D::Alloc (m_nWidth * m_nHeight * sizeof (u16));
		memset (m_pFrameBuffer[i], 0, m_nWidth * m_nHeight * sizeof (u16));
		CV3D::Flush (m_pFrameBuffer[i], m_nWidth * m_nHeight * sizeof (u16));
	}
	m_pDraws = new TDraw[MaxDraws];

	// all fragment shader variants in one block (8-byte instructions)
	unsigned nWords = 0;
	for (unsigned i = 0; i < FRAGMENT_SHADERS; i++)
	{
		nWords += FragmentShaders[i].nInstructions * 2;
	}
	m_pShaderCode = (u32 *) CV3D::Alloc (nWords * 4);

	assert (FRAGMENT_SHADERS <= sizeof m_ShaderBus / sizeof m_ShaderBus[0]);
	u32 *p = m_pShaderCode;
	for (unsigned i = 0; i < FRAGMENT_SHADERS; i++)
	{
		unsigned n = FragmentShaders[i].nInstructions * 2;
		memcpy (p, FragmentShaders[i].pCode, n * 4);
		m_ShaderBus[i] = CV3D::BusAddress (p);
		p += n;
	}
	CV3D::Flush (m_pShaderCode, nWords * 4);

	return TRUE;
}

void CRenderer::AddTriangles (const TDrawSetup &rSetup, const float *pVertices, unsigned nVertices)
{
	assert (rSetup.nShader < FRAGMENT_SHADERS);
	assert (rSetup.nUniforms == FragmentShaders[rSetup.nShader].nUniforms);

	unsigned nVaryings = FragmentShaders[rSetup.nShader].nVaryings;
	unsigned nStride = NVStride (rSetup.nShader);

	// merge with the previous draw if everything matches and the data is contiguous
	boolean bMerge =    m_nDraws > 0
			 && !m_pDraws[m_nDraws-1].bGL
			 && m_pDraws[m_nDraws-1].nShader == rSetup.nShader
			 && m_pDraws[m_nDraws-1].nConfigBits == rSetup.nConfigBits
			 && m_pDraws[m_nDraws-1].nVertexOffset + m_pDraws[m_nDraws-1].nVertices * nStride
				== m_nVertexBytes
			 && memcmp (m_pUniformPool + m_pDraws[m_nDraws-1].nUniformOffset,
				    rSetup.pUniforms, rSetup.nUniforms * 4) == 0;

	if (   m_nVertexBytes + nVertices * nStride > VertexPoolBytes
	    || (!bMerge && (   m_nDraws >= MaxDraws
			    || m_nUniformWords + rSetup.nUniforms > UniformPoolWords)))
	{
		m_nDropped += nVertices / 3;
		return;
	}

	if (bMerge)
	{
		m_pDraws[m_nDraws-1].nVertices += nVertices;
	}
	else
	{
		TDraw &Draw = m_pDraws[m_nDraws++];
		Draw.bGL = FALSE;
		Draw.nShader = rSetup.nShader;
		Draw.nConfigBits = rSetup.nConfigBits;
		Draw.nUniformOffset = m_nUniformWords;
		Draw.nUniforms = rSetup.nUniforms;
		Draw.nVertexOffset = m_nVertexBytes;
		Draw.nVertices = nVertices;

		memcpy (m_pUniformPool + m_nUniformWords, rSetup.pUniforms, rSetup.nUniforms * 4);
		m_nUniformWords += rSetup.nUniforms;
	}

	u8 *p = m_pVertexPool + m_nVertexBytes;
	for (unsigned i = 0; i < nVertices; i++, p += nStride)
	{
		const float *v = pVertices + i * (SCREEN_VERTEX_FIXED + nVaryings);
		CControlList Vertex (p, nStride);
		Vertex.Add16 ((u16) (s16) (v[0] * 16.0f));	// 12.4 fixed point
		Vertex.Add16 ((u16) (s16) (v[1] * 16.0f));
		Vertex.AddFloat (v[2]);
		Vertex.AddFloat (v[3]);
		for (unsigned k = 0; k < nVaryings; k++)
		{
			Vertex.AddFloat (v[SCREEN_VERTEX_FIXED + k]);
		}
	}

	m_nVertexBytes += nVertices * nStride;
}

boolean CRenderer::AddGLDraw (const TGLDraw &rDraw)
{
	if (m_nDraws >= MaxDraws)
	{
		m_nDropped += rDraw.nCount / 3;
		return FALSE;
	}

	TDraw &Draw = m_pDraws[m_nDraws++];
	Draw.bGL = TRUE;
	Draw.GL = rDraw;
	Draw.nConfigBits = rDraw.nConfigBits;

	return TRUE;
}

u8 *CRenderer::AllocRecord (unsigned nBytes, u32 *pBus)
{
	unsigned nOffset = (m_nRecordBytes + 15) & ~15;
	if (nOffset + nBytes > RecordPoolBytes)
	{
		return nullptr;
	}
	m_nRecordBytes = nOffset + nBytes;

	*pBus = CV3D::BusAddress (m_pRecordPool + nOffset);
	return m_pRecordPool + nOffset;
}

u32 *CRenderer::AllocUniforms (unsigned nWords, u32 *pBus)
{
	if (m_nUniformWords + nWords > UniformPoolWords)
	{
		return nullptr;
	}
	u32 *p = m_pUniformPool + m_nUniformWords;
	m_nUniformWords += nWords;

	*pBus = CV3D::BusAddress (p);
	return p;
}

u8 *CRenderer::AllocData (unsigned nBytes, u32 *pBus)
{
	unsigned nOffset = (m_nVertexBytes + 15) & ~15;
	if (nOffset + nBytes > VertexPoolBytes)
	{
		return nullptr;
	}
	m_nVertexBytes = nOffset + nBytes;

	*pBus = CV3D::BusAddress (m_pVertexPool + nOffset);
	return m_pVertexPool + nOffset;
}

boolean CRenderer::EndFrame (boolean bClear, u32 nClearColor, float fClearDepth, TRenderStats *pStats)
{
	CV3D::Flush (m_pVertexPool, m_nVertexBytes);
	CV3D::Flush (m_pUniformPool, m_nUniformWords * 4);
	CV3D::Flush (m_pRecordPool, m_nRecordBytes);

	// NV shader state records, one per draw
	for (unsigned i = 0; i < m_nDraws; i++)
	{
		const TDraw &Draw = m_pDraws[i];
		if (Draw.bGL)
		{
			continue;
		}
		CControlList Rec (m_pRecords + i * 16, 16);
		Rec.Add8 (0x01);				// fragment shader single threaded
		Rec.Add8 (NVStride (Draw.nShader));
		Rec.Add8 (0);
		Rec.Add8 (FragmentShaders[Draw.nShader].nVaryings);
		Rec.Add32 (m_ShaderBus[Draw.nShader]);
		Rec.Add32 (CV3D::BusAddress (m_pUniformPool + Draw.nUniformOffset));
		Rec.Add32 (CV3D::BusAddress (m_pVertexPool + Draw.nVertexOffset));
	}
	CV3D::Flush (m_pRecords, m_nDraws * 16);

	// binning control list
	CControlList Bin (m_pBinCL, BIN_CL_SIZE);
	Bin.Add8 (V3D_TILE_BINNING_MODE_CONFIG);
	Bin.Add32 (CV3D::BusAddress (m_pTileAlloc));
	Bin.Add32 (TILE_ALLOC_SIZE);
	Bin.Add32 (CV3D::BusAddress (m_pTileState));
	Bin.Add8 (m_nTilesX);
	Bin.Add8 (m_nTilesY);
	Bin.Add8 (0x04);			// auto-initialise tile state data array

	Bin.Add8 (V3D_START_TILE_BINNING);

	Bin.Add8 (V3D_CLIP_WINDOW);
	Bin.Add16 (0);
	Bin.Add16 (0);
	Bin.Add16 (m_nWidth);
	Bin.Add16 (m_nHeight);

	Bin.Add8 (V3D_VIEWPORT_OFFSET);
	Bin.Add16 (0);
	Bin.Add16 (0);

	unsigned nTriangles = 0;
	u32 nLastConfig = 0xFFFFFFFF;
	boolean bNVViewport = TRUE;		// clip window and viewport offset for NV draws
	for (unsigned i = 0; i < m_nDraws; i++)
	{
		const TDraw &Draw = m_pDraws[i];

		if (Draw.nConfigBits != nLastConfig)
		{
			Bin.Add8 (V3D_CONFIGURATION_BITS);
			Bin.Add8 (Draw.nConfigBits & 0xFF);
			Bin.Add8 ((Draw.nConfigBits >> 8) & 0xFF);
			Bin.Add8 ((Draw.nConfigBits >> 16) & 0xFF);
			nLastConfig = Draw.nConfigBits;
		}

		if (Draw.bGL)
		{
			const TGLDraw &G = Draw.GL;

			Bin.Add8 (V3D_CLIP_WINDOW);
			Bin.Add16 (G.nClipX);
			Bin.Add16 (G.nClipY);
			Bin.Add16 (G.nClipWidth);
			Bin.Add16 (G.nClipHeight);

			Bin.Add8 (V3D_VIEWPORT_OFFSET);		// 12.4 fixed point
			Bin.Add16 ((u16) (s16) (G.fCentreX * 16.0f));
			Bin.Add16 ((u16) (s16) (G.fCentreY * 16.0f));

			Bin.Add8 (V3D_CLIPPER_XY_SCALING);	// in 1/16 pixel
			Bin.AddFloat (G.fHalfWidth * 16.0f);
			Bin.AddFloat (-G.fHalfHeight * 16.0f);	// y down

			Bin.Add8 (V3D_CLIPPER_Z_SCALE_OFFSET);
			Bin.AddFloat (G.fZScale);
			Bin.AddFloat (G.fZOffset);

			Bin.Add8 (V3D_GL_SHADER_STATE);		// record address | attribute arrays (8 = 0)
			Bin.Add32 (G.nRecordBus | (G.nAttributes & 7));

			if (G.bIndexed)
			{
				Bin.Add8 (V3D_INDEXED_PRIMITIVE_LIST);
				Bin.Add8 (G.nMode | G.nIndexType << 4);
				Bin.Add32 (G.nCount);
				Bin.Add32 (G.nIndexBus);
				Bin.Add32 (G.nMaxIndex);
			}
			else
			{
				Bin.Add8 (V3D_VERTEX_ARRAY_PRIMITIVES);
				Bin.Add8 (G.nMode);
				Bin.Add32 (G.nCount);
				Bin.Add32 (0);			// first vertex: in the attribute addresses
			}

			nTriangles += G.nMode >= 4 ? (G.nMode == 4 ? G.nCount / 3 : G.nCount - 2) : 0;
			bNVViewport = FALSE;
			continue;
		}

		if (!bNVViewport)
		{
			Bin.Add8 (V3D_CLIP_WINDOW);
			Bin.Add16 (0);
			Bin.Add16 (0);
			Bin.Add16 (m_nWidth);
			Bin.Add16 (m_nHeight);

			Bin.Add8 (V3D_VIEWPORT_OFFSET);
			Bin.Add16 (0);
			Bin.Add16 (0);

			bNVViewport = TRUE;
		}

		Bin.Add8 (V3D_NV_SHADER_STATE);
		Bin.Add32 (CV3D::BusAddress (m_pRecords + i * 16));

		Bin.Add8 (V3D_VERTEX_ARRAY_PRIMITIVES);
		Bin.Add8 (V3D_PRIM_TRIANGLES);
		Bin.Add32 (Draw.nVertices);
		Bin.Add32 (0);

		nTriangles += Draw.nVertices / 3;
	}

	Bin.Add8 (V3D_FLUSH);
	Bin.Add8 (V3D_NOP);
	assert (!Bin.Overflow ());
	Bin.Flush ();

	// rendering control list into the buffer that is not being sent
	u16 *pTarget = m_pFrameBuffer[m_nBuffer];
	u16 *pPrevious = m_pFrameBuffer[m_nBuffer ^ 1];

	// the clear colour is in the tile buffer's 32-bit format (R in byte 0,
	// like the shaders), also with the RGB565 store (verified on the panel)
	float fDepth = fClearDepth < 0.0f ? 0.0f : fClearDepth > 1.0f ? 1.0f : fClearDepth;
	u32 nClearZ = (u32) (fDepth * 0xFFFFFF);

	CControlList Render (m_pRenderCL, RENDER_CL_SIZE);
	Render.Add8 (V3D_CLEAR_COLORS);
	Render.Add32 (nClearColor);
	Render.Add32 (nClearColor);
	Render.Add16 (nClearZ & 0xFFFF);	// clear Z (24 bits) ...
	Render.Add8 (nClearZ >> 16);
	Render.Add8 (0);			// ... clear VG mask
	Render.Add8 (0);			// clear stencil

	Render.Add8 (V3D_TILE_RENDERING_MODE_CONFIG);
	Render.Add32 (CV3D::BusAddress (pTarget));
	Render.Add16 (m_nWidth);
	Render.Add16 (m_nHeight);
	Render.Add16 (0 << 2);			// BGR565 dithered, linear

	Render.Add8 (V3D_TILE_COORDINATES);	// dummy store to clear the tile buffer
	Render.Add8 (0);
	Render.Add8 (0);
	Render.Add8 (V3D_STORE_TILE_BUFFER_GENERAL);
	Render.Add16 (0);
	Render.Add32 (0);

	for (unsigned y = 0; y < m_nTilesY; y++)
	{
		for (unsigned x = 0; x < m_nTilesX; x++)
		{
			if (!bClear)
			{
				// the load happens when the tile coordinates are processed
				Render.Add8 (V3D_LOAD_TILE_BUFFER_GENERAL);
				Render.Add16 (LOADSTORE_BUFFER_COLOR | LOADSTORE_TILING_RASTER
					      | LOADSTORE_FORMAT_BGR565);
				Render.Add32 (CV3D::BusAddress (pPrevious));
			}

			Render.Add8 (V3D_TILE_COORDINATES);
			Render.Add8 (x);
			Render.Add8 (y);

			Render.Add8 (V3D_BRANCH_TO_SUBLIST);
			Render.Add32 (CV3D::BusAddress (m_pTileAlloc)
				      + (y * m_nTilesX + x) * V3D_TILE_ALLOC_BLOCK);

			boolean bLast = x == m_nTilesX-1 && y == m_nTilesY-1;
			Render.Add8 (bLast ? V3D_STORE_MS_TILE_BUFFER_EOF : V3D_STORE_MS_TILE_BUFFER);
		}
	}
	assert (!Render.Overflow ());
	Render.Flush ();

	unsigned nBinUs = 0, nRenderUs = 0;
	boolean bOK = m_pV3D->RunJob (Bin.GetStartBus (), Bin.GetEndBus (),
				      Render.GetStartBus (), Render.GetEndBus (),
				      CV3D::BusAddress (m_pOverflow), OVERFLOW_SIZE,
				      &nBinUs, &nRenderUs);

	// present: wait for the previous frame's DMA, then send this one
	unsigned nStart = CTimer::GetClockTicks ();
	m_pDisplay->WaitIdle ();
	unsigned nWaitUs = CTimer::GetClockTicks () - nStart;

	const CDisplay::TArea Full = {0, m_nWidth-1, 0, m_nHeight-1};
	m_pDisplay->SetArea (Full, pTarget, PanelDone, this);
	m_nBuffer ^= 1;

	m_PinFrame.Write (HIGH);		// FRAME pulse (>= 10 us)
	CTimer::SimpleusDelay (10);
	m_PinFrame.Write (LOW);

	if (pStats)
	{
		pStats->nDraws = m_nDraws;
		pStats->nTriangles = nTriangles;
		pStats->nDroppedTriangles = m_nDropped;
		pStats->nRenderUs = nBinUs + nRenderUs;
		pStats->nPresentWaitUs = nWaitUs;
	}

	m_nDraws = 0;
	m_nVertexBytes = 0;
	m_nUniformWords = 0;
	m_nRecordBytes = 0;
	m_nDropped = 0;

	return bOK;
}

void CRenderer::DiscardFrame (void)
{
	m_nDraws = 0;
	m_nVertexBytes = 0;
	m_nUniformWords = 0;
	m_nRecordBytes = 0;
	m_nDropped = 0;
}

void CRenderer::PanelDone (void *pParam)
{
	// WaitIdle () synchronizes; a routine makes SetArea () asynchronous
}

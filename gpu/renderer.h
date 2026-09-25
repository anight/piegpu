//
// renderer.h
//
// Per-frame V3D renderer for the command executor. Collects screen-space
// triangles (NV mode, transformed by the ARM) together with a fragment shader
// variant and its uniforms, renders them into one of two RGB565 panel buffers
// and presents that buffer by DMA. Pulses FRAME (GPIO26) when a frame is
// handed to the panel.
//
#ifndef _renderer_h
#define _renderer_h

#include <v3d.h>
#include <st7789dma.h>
#include <circle/gpiopin.h>
#include <circle/types.h>

// A vertex as given to AddTriangles (): 4 + nVaryings floats
//   x, y   panel pixels (y down)
//   z      window depth 0 .. 1
//   1/w    for perspective-correct varyings
//   varyings of the shader variant: r, g, b, a [, s, t] [, fog]
#define SCREEN_VERTEX_FIXED	4
#define MAX_VARYINGS		7

struct TDrawSetup
{
	unsigned nShader;		// index into FragmentShaders[]
	u32 nConfigBits;		// V3D configuration bits
	const u32 *pUniforms;		// resolved uniform values
	unsigned nUniforms;
};

// A draw in the V3D's GL shader mode (programs, docs/protocol.md 7.10). The
// shader record, uniform streams and constant attribute data are placed in
// the frame's pools with AllocRecord (), AllocUniforms () and AllocData ().
struct TGLDraw
{
	u32 nConfigBits;
	u32 nRecordBus;			// shader record, 16-byte aligned
	unsigned nAttributes;		// attribute records after it (1-8)

	// viewport (panel pixels, y down)
	float fCentreX, fCentreY;
	float fHalfWidth, fHalfHeight;	// the y scale is -fHalfHeight
	float fZScale, fZOffset;
	unsigned nClipX, nClipY, nClipWidth, nClipHeight;

	u8 nMode;			// primitive mode (same values as GL)
	boolean bIndexed;
	u32 nCount;
	u32 nIndexBus;			// indexed: index list
	u8 nIndexType;			// 0 u8, 1 u16
	u32 nMaxIndex;
};

struct TRenderStats
{
	unsigned nDraws;
	unsigned nTriangles;
	unsigned nDroppedTriangles;	// pools full
	unsigned nRenderUs;
	unsigned nPresentWaitUs;
};

class CRenderer
{
public:
	static const unsigned MaxDraws = 4096;			// per frame
	static const unsigned VertexPoolBytes = 4 * 1024 * 1024;
	static const unsigned UniformPoolWords = 256 * 1024;
	static const unsigned RecordPoolBytes = 512 * 1024;

public:
	CRenderer (CV3D *pV3D, CST7789DMADisplay *pDisplay);
	~CRenderer (void);

	boolean Initialize (void);

	unsigned GetWidth (void) const		{ return m_nWidth; }
	unsigned GetHeight (void) const		{ return m_nHeight; }

	/// \return Number of varyings of a shader variant
	static unsigned GetVaryings (unsigned nShader);

	/// \brief Add a triangle list (3 vertices per triangle) to the current frame
	/// \param pVertices 4 + GetVaryings (nShader) floats per vertex
	void AddTriangles (const TDrawSetup &rSetup, const float *pVertices, unsigned nVertices);

	/// \brief Add a GL shader mode draw (see TGLDraw)
	/// \return FALSE if the frame is full (the draw is dropped)
	boolean AddGLDraw (const TGLDraw &rDraw);

	/// \brief Space in the current frame's pools, nullptr if full
	u8 *AllocRecord (unsigned nBytes, u32 *pBus);	// 16-byte aligned
	u32 *AllocUniforms (unsigned nWords, u32 *pBus);
	u8 *AllocData (unsigned nBytes, u32 *pBus);	// 16-byte aligned, in the vertex pool

	/// \brief Render the collected frame and hand it to the panel
	/// \param bClear FALSE: start from the previous frame's image (colour only)
	/// \param nClearColor RGBA8888 (0xAABBGGRR)
	/// \param fClearDepth 0 .. 1
	boolean EndFrame (boolean bClear, u32 nClearColor, float fClearDepth, TRenderStats *pStats);

	/// \brief Drop the draws collected for the current frame
	void DiscardFrame (void);

	/// \return The frame last handed to the panel (RGB565, little endian)
	const u16 *GetLastFrame (void) const	{ return m_pFrameBuffer[m_nBuffer ^ 1]; }

private:
	static void PanelDone (void *pParam);

private:
	CV3D *m_pV3D;
	CST7789DMADisplay *m_pDisplay;
	CGPIOPin m_PinFrame;

	unsigned m_nWidth;
	unsigned m_nHeight;
	unsigned m_nTilesX;
	unsigned m_nTilesY;

	u8 *m_pBinCL;
	u8 *m_pRenderCL;
	u8 *m_pRecords;			// NV shader state records, 16 bytes each
	u8 *m_pRecordPool;		// GL shader records
	unsigned m_nRecordBytes;
	u32 *m_pShaderCode;
	u32 m_ShaderBus[64];		// bus address of each variant's code
	u8 *m_pVertexPool;
	u32 *m_pUniformPool;
	u8 *m_pTileAlloc;
	u8 *m_pTileState;
	u8 *m_pOverflow;
	u16 *m_pFrameBuffer[2];
	unsigned m_nBuffer;

	struct TDraw
	{
		boolean bGL;
		TGLDraw GL;

		// NV (fixed function)
		unsigned nShader;
		u32 nConfigBits;
		unsigned nUniformOffset;	// words into the uniform pool
		unsigned nUniforms;
		unsigned nVertexOffset;		// bytes into the vertex pool
		unsigned nVertices;
	};
	TDraw *m_pDraws;
	unsigned m_nDraws;
	unsigned m_nVertexBytes;
	unsigned m_nUniformWords;
	unsigned m_nDropped;
};

#endif

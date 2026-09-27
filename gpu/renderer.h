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
#include "display/output.h"
#include <circle/gpiopin.h>
#include <circle/types.h>

// A vertex as given to AddTriangles (): 4 + nVaryings floats
//   x, y   panel pixels (y down)
//   z      window depth 0 .. 1
//   1/w    for perspective-correct varyings
//   varyings of the shader variant: r, g, b, a [, s, t] [, fog]
#define SCREEN_VERTEX_FIXED	4
#define MAX_VARYINGS		7

// Per-draw state shared by both kinds of draws
struct TDrawState
{
	u32 nConfigBits;		// V3D configuration bits
	u16 nClipX, nClipY;		// clip window (scissor), panel pixels, y down
	u16 nClipWidth, nClipHeight;
	u32 nDepthOffset;		// DEPTH_OFFSET packet: factor | units << 16 (float 1-8-7)
	float fLineWidth;		// GL shader mode lines
};

struct TDrawSetup
{
	unsigned nShader;		// index into FragmentShaders[]
	TDrawState State;
	const u32 *pUniforms;		// resolved uniform values
	unsigned nUniforms;
};

// A draw in the V3D's GL shader mode (programs, docs/protocol.md 7.10). The
// shader record, uniform streams and constant attribute data are placed in
// the frame's pools with AllocRecord (), AllocUniforms () and AllocData ().
struct TGLDraw
{
	TDrawState State;		// the clip window includes the viewport
	u32 nRecordBus;			// shader record, 16-byte aligned
	unsigned nAttributes;		// attribute records after it (1-8)

	// viewport (panel pixels, y down)
	float fCentreX, fCentreY;
	float fHalfWidth, fScaleY;	// fScaleY: -half height for rows top down
	float fZScale, fZOffset;

	u8 nMode;			// primitive mode (same values as GL)
	boolean bIndexed;
	u32 nCount;
	u32 nIndexBus;			// indexed: index list
	u8 nIndexType;			// 0 u8, 1 u16
	u32 nMaxIndex;
};

// where a job renders to
struct TRenderTarget
{
	u32 nColorBus;			// colour buffer (store address)
	unsigned nWidth, nHeight;
	u16 nModeFlags;			// TILE_RENDERING_MODE_CONFIG flags: colour and memory format
	u16 nLoadStore;			// LOAD_TILE_BUFFER_GENERAL bits for the colour buffer
	u32 nZSBus;			// depth and stencil storage (T-format), 0: none
};

struct TJobClear
{
	u32 nColor;			// RGBA8888 (0xAABBGGRR)
	float fDepth;			// 0 .. 1
	u8 nStencil;
};

// depth and stencil storage (T-format, 4 bytes per pixel, generously padded)
#define DEPTH_BUFFER_SIZE(w, h)		((((w) + 127) & ~127) * (((h) + 127) & ~127) * 4)

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
	static const unsigned MaxWidth = 1920;			// the screen (SetOutput)
	static const unsigned MaxHeight = 1200;
	static const unsigned MaxPixels = 1920 * 1200;
	static const unsigned MaxBuffers = 3;			// the output's (HDMI: three pages)

public:
	CRenderer (CV3D *pV3D);
	~CRenderer (void);

	/// \param pOutput where frames go first (its size is the screen's)
	boolean Initialize (COutput *pOutput);

	unsigned GetWidth (void) const		{ return m_nWidth; }
	unsigned GetHeight (void) const		{ return m_nHeight; }

	/// \brief Show the frames on another output, or at another size (between
	///	   frames: waits for the current output); both buffers are cleared
	/// \return FALSE if the output can't have the size (nothing changes then)
	boolean SetOutput (COutput *pOutput, unsigned nWidth, unsigned nHeight);
	COutput *GetOutput (void) const		{ return m_pOutput; }

	/// \return Number of varyings of a shader variant
	static unsigned GetVaryings (unsigned nShader);

	/// \brief Add a triangle list (3 vertices per triangle) to the current frame
	/// \param pVertices 4 + GetVaryings (nShader) floats per vertex
	/// \return FALSE if the frame is full (nothing added)
	boolean AddTriangles (const TDrawSetup &rSetup, const float *pVertices, unsigned nVertices);

	/// \brief Count triangles that didn't fit (for the stats)
	void Drop (unsigned nTriangles)		{ m_nDropped += nTriangles; }

	/// \brief Add a GL shader mode draw (see TGLDraw)
	/// \return FALSE if the frame is full (the draw is dropped)
	boolean AddGLDraw (const TGLDraw &rDraw);

	/// \brief Space in the current frame's pools, nullptr if full
	u8 *AllocRecord (unsigned nBytes, u32 *pBus);	// 16-byte aligned
	u32 *AllocUniforms (unsigned nWords, u32 *pBus);
	u8 *AllocData (unsigned nBytes, u32 *pBus);	// 16-byte aligned, in the vertex pool

	/// \brief The panel's back buffer as a target, and the previous frame's image
	void GetPanelTarget (TRenderTarget *pTarget, u32 *pPreviousBus) const;

	/// \brief Render the collected draws into a target (a job); the stats add up
	/// \param nLoadColorBus colour to start from (target's format), 0: clear
	/// \param bLoadZS start from the stored depth and stencil, else clear
	boolean RenderJob (const TRenderTarget &rTarget, u32 nLoadColorBus, boolean bLoadZS,
			   const TJobClear &rClear, TRenderStats *pStats);

	/// \brief Hand the back buffer to the panel (FRAME pulse) and swap buffers
	void Present (TRenderStats *pStats);

	/// \return Draws collected for the next job
	unsigned GetDraws (void) const		{ return m_nDraws; }

	/// \return The back buffer (RGB565), for read-back
	const u16 *GetBackBuffer (void) const	{ return m_pFrameBuffer[m_nBuffer]; }

	/// \brief Drop the draws collected for the current frame
	void DiscardFrame (void);

	/// \return The frame last handed to the panel (RGB565, little endian)
	const u16 *GetLastFrame (void) const	{ return m_pFrameBuffer[Previous ()]; }

private:
	void UseBuffers (void);
	void ClearFrameBuffers (void);
	unsigned Previous (void) const		{ return (m_nBuffer + m_nBuffers - 1) % m_nBuffers; }
	static void PanelDone (void *pParam);

private:
	CV3D *m_pV3D;
	COutput *m_pOutput;
	CGPIOPin m_PinFrame;

	unsigned m_nWidth;
	unsigned m_nHeight;

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
	u16 *m_pOwnBuffer[2];		// for outputs without buffers (the panel)
	u16 *m_pFrameBuffer[MaxBuffers];	// drawn in turn: the output's or m_pOwnBuffer
	unsigned m_nBuffers;
	u32 *m_pDepthBuffer;		// the panel's depth and stencil (T-format)
	unsigned m_nBuffer;		// the back buffer

	struct TDraw
	{
		boolean bGL;
		TGLDraw GL;
		TDrawState State;

		// NV (fixed function)
		unsigned nShader;
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

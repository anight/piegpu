//
// commands.h
//
// Executes protocol commands (docs/protocol.md sections 6-7): keeps the GL
// ES 1.1-style state, buffers and vertex arrays, fetches vertices and hands
// them to the geometry pipeline. Sends replies (section 9) via the receiver.
//
#ifndef _commands_h
#define _commands_h

#include "renderer.h"
#include "textures.h"
#include "geometry.h"
#include "receiver.h"
#include "hostlink.h"
#include "programs.h"
#include <circle/types.h>

struct TCommandStats
{
	unsigned nFrames;
	unsigned nCommands;
	unsigned nErrors;
	u32 nLastError;			// code << 8 | opcode
	unsigned nDraws;		// renderer draw records
	unsigned nProgramDraws;
	unsigned nTriangles;		// rendered (after clipping, incl. line/point quads)
	unsigned nPrimitives;		// submitted
	unsigned nClipped;
	unsigned nRejected;
	unsigned nDropped;		// renderer pools full
	unsigned nRenderUs;		// sum over frames
	unsigned nPresentWaitUs;
};

// the last measuring window (about a second), for STATUS (docs/protocol.md 9)
struct TLoadStats
{
	u32 nWindowUs;
	u32 nFrames;
	u32 nV3DBusyUs;			// binning and rendering
	u32 nARMBusyUs;			// receiving and executing commands, not waiting
	u32 nPanelWaitUs;		// for the panel's DMA of the previous frame
};

class CCommands
{
public:
	static const unsigned MaxBuffers = 256;
	static const unsigned MaxBufferBytes = 16 * 1024 * 1024;

public:
	CCommands (CRenderer *pRenderer, CReceiver *pReceiver);
	~CCommands (void);

	/// \brief RESET: delete all objects, default state, discard the frame
	void Reset (void);

	/// \brief Send INFO (after boot)
	void SendInfo (void);

	/// \brief Replies go to the host's USB stream from now on (the host drives
	/// the GPU instead of the Pico)
	void SetHostLink (CHostLink *pHostLink)		{ m_pHostLink = pHostLink; }

	/// \brief Execute one command packet
	void Execute (u32 nHeader, const u32 *pPayload);

	TCommandStats GetStats (void);
	void SetLoadStats (const TLoadStats &rLoad)	{ m_Load = rLoad; }

private:
	struct TBuffer
	{
		u8 *pData;			// nullptr: no such buffer
		unsigned nSize;
		boolean bUsed;			// read by the V3D in the current frame
	};

	struct TGenericArray
	{
		u32 nBuffer;
		unsigned nOffset;
		unsigned nStride;		// as given (0 = tightly packed)
		unsigned nSize;
		u32 nType;
	};

	struct TViewport
	{
		float fCentreX, fCentreY, fHalfWidth, fScaleY, fZScale, fZOffset;	// fScaleY signed
		unsigned nClipX, nClipY, nClipWidth, nClipHeight;
	};

	struct TArray
	{
		u32 nBuffer;
		unsigned nOffset;
		unsigned nStride;
		unsigned nSize;			// components
		u32 nType;
	};

	void DefaultState (void);
	u32 Dispatch (u32 nOpcode, const u32 *p, unsigned nLength, u32 *pDetail);

	u32 BufferCreate (u32 nId, unsigned nSize);
	u32 BufferData (const u32 *p, unsigned nLength);
	u32 BufferDelete (u32 nId);

	u32 DrawArrays (u32 nMode, unsigned nFirst, unsigned nCount);
	u32 DrawElements (u32 nMode, unsigned nCount, u32 nIndexType, u32 nBuffer, unsigned nOffset);
	u32 DrawInline (const u32 *p, unsigned nLength);
	u32 FetchVertices (unsigned nFirst, unsigned nCount);
	u32 Draw (u32 nMode, unsigned nVertices, const u32 *pIndices, unsigned nCount);

	// vertex (and index) data carried in a PROGRAM_DRAW_INLINE packet
	struct TInlineData
	{
		u32 nMask;			// attributes in the packet
		const u8 *pAttribute[PGPU_MAX_ATTRIBUTES];
		u32 Format[PGPU_MAX_ATTRIBUTES];	// type | size << 8
		const u8 *pIndices;		// nullptr: not indexed
		unsigned nIndexBytes;
	};

	u32 ProgramDraw (u32 nMode, unsigned nFirst, unsigned nCount,
			 boolean bIndexed, u32 nIndexType, u32 nIndexBuffer, unsigned nIndexOffset,
			 u32 *pDetail, const TInlineData *pInline = nullptr, unsigned nVertices = 0);
	u32 ProgramDrawInline (const u32 *p, unsigned nLength, u32 *pDetail);
	boolean GetViewport (TViewport *pViewport) const;
	u32 *BuildUniforms (const TProgram *pProgram, const TProgramShader *pShader, u32 *pStorageBus,
			    const TViewport &rViewport, u32 *pBus);
	u32 BlendMode (void) const;
	u32 ConfigBits (boolean bFaces) const;
	void RetireBuffer (TBuffer *pBuffer);
	void EndFrame (u32 nFlags);

	void DefaultInput (TInputVertex *pVertex) const;
	boolean Reply (u8 uchOpcode, const u32 *pPayload, unsigned nLength);
	void FlushIfTarget (u32 nTexture);
	void Error (u32 nCode, u32 nOpcode, u32 nDetail);

private:
	CRenderer *m_pRenderer;
	CReceiver *m_pReceiver;
	CHostLink *m_pHostLink;
	CTextures m_Textures;
	CGeometry m_Geometry;

	TGLState m_State;

	TBuffer m_Buffers[MaxBuffers + 1];	// ids 1 .. MaxBuffers
	unsigned m_nBufferBytes;

	TArray m_Arrays[4];
	u32 m_nArraysEnabled;

	static const unsigned MaxRetiredBuffers = 64;
	u8 *m_RetiredBuffers[MaxRetiredBuffers];
	unsigned m_nRetiredBuffers;

	// programs (GL ES 2.0 subset)
	CPrograms m_Programs;
	u32 m_nProgram;			// in use, 0 = fixed function
	TGenericArray m_Generic[PGPU_MAX_ATTRIBUTES];
	u32 m_nGenericEnabled;
	float m_GenericValue[PGPU_MAX_ATTRIBUTES][4];
	u32 m_TextureUnit[PGPU_MAX_TEXTURE_UNITS];	// unit 0 is also m_State.nBoundTexture
	float m_BlendK[4][12];			// blend coefficients of the current program draw

	TInputVertex *m_pInput;			// vertex fetch buffer
	u32 *m_pIndices;

	// framebuffer objects (render to texture)
	struct TZSBuffer			// depth and stencil (allocated when rendered)
	{
		u8 *p;
		unsigned nBytes;
		boolean bValid;			// stored by a job
	};
	struct TFramebuffer
	{
		boolean bValid;
		u32 nTexture;			// colour: level 0 of this texture
		unsigned nFace;			// cube map face
		boolean bDepthStencil;
		unsigned nSharedZS;		// 1 .. MaxSharedZS: shared depth and stencil
		TZSBuffer ZS;			// else its own
	};
	static const unsigned MaxFramebuffers = 16;
	TFramebuffer m_Framebuffers[MaxFramebuffers + 1];	// ids 1 .. MaxFramebuffers
	static const unsigned MaxSharedZS = 16;
	TZSBuffer m_SharedZS[MaxSharedZS + 1];			// ids 1 .. MaxSharedZS
	TZSBuffer &ZSOf (TFramebuffer &F)	{ return F.nSharedZS ? m_SharedZS[F.nSharedZS] : F.ZS; }
	u32 m_nFramebuffer;		// bound, 0 = the panel

	// the job: what the renderer collects for the bound target
	boolean m_bJobPending;		// draws or a clear to render
	boolean m_bJobClearColor, m_bJobClearZS;
	TJobClear m_JobClear;
	boolean m_bPanelDrawn;		// a job rendered into the back buffer this frame
	boolean m_bPanelZSValid;
	TRenderStats m_FrameStats;

	void UpdateTarget (void);
	void FlushJob (boolean bForce);
	static void JobFullHandler (void *pParam);
	void EndJob (void);
	u32 Clear (u32 nMask, u32 nColor, float fDepth, u8 nStencil);
	u32 ReadRect (s32 x, s32 y, unsigned nWidth, unsigned nHeight, u32 *pOut);
	u32 FramebufferCreate (u32 nId, u32 nTexture, u32 nFlags);
	void FramebufferFree (TFramebuffer *pFramebuffer);
	unsigned m_nFrameNumber;
	unsigned m_nLastFrameUs;
	TLoadStats m_Load;

	// totals for STATUS
	unsigned m_nTotalFrames;
	unsigned m_nTotalErrors;

	TCommandStats m_Stats;
};

#endif

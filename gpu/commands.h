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
#include <circle/types.h>

struct TCommandStats
{
	unsigned nFrames;
	unsigned nCommands;
	unsigned nErrors;
	u32 nLastError;			// code << 8 | opcode
	unsigned nDraws;		// renderer draw records
	unsigned nTriangles;		// rendered (after clipping, incl. line/point quads)
	unsigned nPrimitives;		// submitted
	unsigned nClipped;
	unsigned nRejected;
	unsigned nDropped;		// renderer pools full
	unsigned nRenderUs;		// sum over frames
	unsigned nPresentWaitUs;
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

	/// \brief Execute one command packet
	void Execute (u32 nHeader, const u32 *pPayload);

	TCommandStats GetStats (void);

private:
	struct TBuffer
	{
		u8 *pData;			// nullptr: no such buffer
		unsigned nSize;
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

	void EndFrame (u32 nFlags);

	void DefaultInput (TInputVertex *pVertex) const;
	void Reply (u8 uchOpcode, const u32 *pPayload, unsigned nLength);
	void Error (u32 nCode, u32 nOpcode, u32 nDetail);

private:
	CRenderer *m_pRenderer;
	CReceiver *m_pReceiver;
	CTextures m_Textures;
	CGeometry m_Geometry;

	TGLState m_State;

	TBuffer m_Buffers[MaxBuffers + 1];	// ids 1 .. MaxBuffers
	unsigned m_nBufferBytes;

	TArray m_Arrays[4];
	u32 m_nArraysEnabled;

	TInputVertex *m_pInput;			// vertex fetch buffer
	u32 *m_pIndices;

	// frame
	boolean m_bFrameHasDraw;
	boolean m_bClearColor;
	u32 m_nClearColor;
	float m_fClearDepth;
	unsigned m_nFrameNumber;
	unsigned m_nLastFrameUs;

	// totals for STATUS
	unsigned m_nTotalFrames;
	unsigned m_nTotalErrors;

	TCommandStats m_Stats;
};

#endif

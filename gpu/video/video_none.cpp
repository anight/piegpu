//
// video_none.cpp
//
// CVideo for builds without the VideoCore's services (gpu/Makefile: VIDEO=0).
// Opening a stream answers as video.cpp does when MMAL isn't there; the rest
// has nothing to do.
//
#include "video.h"
#include <pgpu_protocol.h>
#include <circle/logger.h>
#include <circle/util.h>

LOGMODULE ("video");

CVideo::CVideo (CTextures *pTextures)
:	m_pTextures (pTextures),
	m_bInitialized (FALSE)
{
	memset (m_Streams, 0, sizeof m_Streams);
}

CVideo::~CVideo (void)
{
}

boolean CVideo::Initialize (void)
{
	LOGNOTE ("No video in this build (%u bit)", AARCH);

	return TRUE;
}

u32 CVideo::Open (unsigned nStream, u32 nCodec, u32 nTexture, unsigned nWidth, unsigned nHeight,
		  unsigned nCodedWidth, unsigned nCodedHeight, u32 nFormat, const u8 *pConfig,
		  unsigned nConfigBytes)
{
	if (nStream < 1 || nStream > MaxStreams)
	{
		return PGPU_ERR_ID;
	}
	if (   nCodec != PGPU_VIDEO_H264 || !nCodedWidth || !nCodedHeight
	    || (nFormat != PGPU_VIDEO_ANNEXB && nFormat != PGPU_VIDEO_AVCC)
	    || (nFormat == PGPU_VIDEO_AVCC && !nConfigBytes))
	{
		return PGPU_ERR_ENUM;
	}

	return PGPU_ERR_MEMORY;			// (as without MMAL)
}

u32 CVideo::Data (unsigned nStream, u32 nFlags, s64 nPTS, unsigned nSampleBytes,
		  const u8 *pData, unsigned nBytes)
{
	return PGPU_ERR_OBJECT;			// (no stream is open)
}

u32 CVideo::Control (unsigned nStream, u32 nOp, s64 nArg)
{
	return PGPU_ERR_OBJECT;
}

void CVideo::Close (unsigned nStream)
{
}

u32 CVideo::Resize (unsigned nStream, unsigned nWidth, unsigned nHeight)
{
	return PGPU_ERR_OBJECT;
}

void CVideo::CloseAll (void)
{
}

void CVideo::Update (void)
{
}

void CVideo::FrameEnd (void)
{
}

boolean CVideo::GetStatus (unsigned nStream, u32 *pPayload, boolean bDue)
{
	return FALSE;
}

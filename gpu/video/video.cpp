//
// video.cpp
//
#include "video.h"
#include <v3d.h>
#include <pgpu_protocol.h>
#include <circle/logger.h>
#include <circle/timer.h>
#include <circle/util.h>
#include <assert.h>

extern "C" {
#include "interface/mmal/mmal.h"
#include "interface/mmal/util/mmal_util.h"
#include "interface/mmal/util/mmal_util_params.h"
#include "interface/mmal/util/mmal_connection.h"
#include "interface/mmal/vc/mmal_vc_api.h"
}

#define INPUT_BUFFER_BYTES	(64 * 1024)	// the decoder's input buffers
#define OUTPUT_BUFFERS		(CVideo::MaxFrames + 2)	// waiting, shown, being filled
#define STATUS_US		100000		// periodic VIDEO_STATUS

LOGMODULE ("video");

CVideo::CVideo (CTextures *pTextures)
:	m_pTextures (pTextures),
	m_bInitialized (FALSE)
{
	memset (m_Streams, 0, sizeof m_Streams);
}

CVideo::~CVideo (void)
{
	CloseAll ();
}

boolean CVideo::Initialize (void)
{
	MMAL_STATUS_T s = mmal_vc_init ();
	if (s != MMAL_SUCCESS)
	{
		LOGWARN ("No MMAL (%d): no video", (int) s);
		return FALSE;
	}
	m_bInitialized = TRUE;
	LOGNOTE ("MMAL connected to the VideoCore");

	return TRUE;
}

// ---- MMAL's callbacks (VCHIQ's tasks) --------------------------------------------------

void CVideo::ControlCallback (MMAL_PORT_T *pPort, MMAL_BUFFER_HEADER_T *pBuffer)
{
	if (pBuffer->cmd == MMAL_EVENT_ERROR)
	{
		TStream *pStream = (TStream *) pPort->userdata;
		pStream->bError = TRUE;
		LOGWARN ("%s: error %d", pPort->component->name, *(int *) pBuffer->data);
	}
	mmal_buffer_header_release (pBuffer);
}

void CVideo::InputCallback (MMAL_PORT_T *pPort, MMAL_BUFFER_HEADER_T *pBuffer)
{
	mmal_buffer_header_release (pBuffer);		// back to the input pool
}

void CVideo::OutputCallback (MMAL_PORT_T *pPort, MMAL_BUFFER_HEADER_T *pBuffer)
{
	TStream *pStream = (TStream *) pPort->userdata;
	mmal_queue_put (pStream->pDecoded, pBuffer);
}

// the ISP's frames: 4 KB aligned (the V3D's texture base), the allocation's
// start kept in the word before
static void *AllocFrame (void *pContext, uint32_t nSize)
{
	u8 *pRaw = CV3D::NewBlock (nSize + 4096 + sizeof (u8 *));
	if (!pRaw)
	{
		return nullptr;
	}
	u8 *p = (u8 *) (((uintptr) pRaw + sizeof (u8 *) + 4095) & ~(uintptr) 4095);
	((u8 **) p)[-1] = pRaw;

	return p;
}

static void FreeFrame (void *pContext, void *pMem)
{
	if (pMem)
	{
		CV3D::DeleteBlock (((u8 **) pMem)[-1]);
	}
}

// ---- commands -------------------------------------------------------------------------

#define CHECK(s, what)								\
	if ((s) != MMAL_SUCCESS)						\
	{									\
		LOGWARN ("Stream %u: %s: status %d", nStream, what, (int) (s));	\
		Close (nStream);						\
		return PGPU_ERR_MEMORY;						\
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
	if (!m_bInitialized)
	{
		return PGPU_ERR_MEMORY;
	}
	u32 nError = m_pTextures->CreateExternal (nTexture, nWidth, nHeight);
	if (nError)
	{
		return nError;
	}
	Close (nStream);

	TStream &S = m_Streams[nStream];
	memset (&S, 0, sizeof S);
	S.nTexture = nTexture;
	S.nWidth = nWidth;
	S.nHeight = nHeight;
	S.nPausedAt = MMAL_TIME_UNKNOWN;
	S.bOpen = TRUE;			// from here on Close cleans up

	S.pRing = new u8[RingBytes];
	S.pDecoded = mmal_queue_create ();
	if (!S.pRing || !S.pDecoded)
	{
		Close (nStream);
		return PGPU_ERR_MEMORY;
	}

	MMAL_STATUS_T s = mmal_component_create ("vc.ril.video_decode", &S.pDecoder);
	CHECK (s, "create vc.ril.video_decode");
	s = mmal_component_create ("vc.ril.isp", &S.pISP);
	CHECK (s, "create vc.ril.isp");
	S.pDecoder->control->userdata = (MMAL_PORT_USERDATA_T *) &S;
	S.pISP->control->userdata = (MMAL_PORT_USERDATA_T *) &S;
	mmal_port_enable (S.pDecoder->control, ControlCallback);
	mmal_port_enable (S.pISP->control, ControlCallback);

	// the decoder: H.264, framed samples with their times
	MMAL_PORT_T *pIn = S.pDecoder->input[0];
	MMAL_ES_FORMAT_T *pFormat = pIn->format;
	pFormat->type = MMAL_ES_TYPE_VIDEO;
	pFormat->encoding = MMAL_ENCODING_H264;
	pFormat->es->video.width = nCodedWidth;
	pFormat->es->video.height = nCodedHeight;
	pFormat->flags = MMAL_ES_FORMAT_FLAG_FRAMED;
	if (nFormat == PGPU_VIDEO_AVCC)		// as MP4 has it: the decoder takes the avcC
	{
		pFormat->encoding_variant = MMAL_ENCODING_VARIANT_H264_AVC1;
		s = mmal_format_extradata_alloc (pFormat, nConfigBytes);
		CHECK (s, "the avcC");
		memcpy (pFormat->extradata, pConfig, nConfigBytes);
		pFormat->extradata_size = nConfigBytes;
	}
	s = mmal_port_format_commit (pIn);
	CHECK (s, "decoder input format");
	// its pictures go to the ISP in the VideoCore's own format: as I420 the
	// ISP returned empty frames whenever the decoder's row (the width rounded
	// up to 32) wasn't a multiple of 64 (720, 800, 854 wide: the chroma rows
	// not a multiple of 32)
	S.pDecoder->output[0]->format->encoding = MMAL_ENCODING_OPAQUE;
	s = mmal_port_format_commit (S.pDecoder->output[0]);
	CHECK (s, "decoder output format");

	// decoder -> ISP inside the VideoCore
	s = mmal_connection_create (&S.pConnection, S.pDecoder->output[0], S.pISP->input[0],
				    MMAL_CONNECTION_FLAG_TUNNELLING);
	CHECK (s, "connect the decoder to the ISP");
	s = mmal_connection_enable (S.pConnection);
	CHECK (s, "enable the connection");

	// the ISP: the texture's size, RGBA (R in byte 0), the whole picture
	MMAL_PORT_T *pOut = S.pISP->output[0];
	mmal_format_copy (pOut->format, S.pISP->input[0]->format);
	pOut->format->encoding = MMAL_ENCODING_RGBA;
	pOut->format->es->video.width = nWidth;
	pOut->format->es->video.height = nHeight;
	pOut->format->es->video.crop.x = 0;
	pOut->format->es->video.crop.y = 0;
	pOut->format->es->video.crop.width = nWidth;
	pOut->format->es->video.crop.height = nHeight;
	s = mmal_port_format_commit (pOut);
	CHECK (s, "ISP output format");

	pIn->buffer_num = pIn->buffer_num_recommended;
	pIn->buffer_size = INPUT_BUFFER_BYTES;
	pOut->buffer_num = OUTPUT_BUFFERS;
	pOut->buffer_size = pOut->buffer_size_recommended;
	assert (pOut->buffer_size >= nWidth * nHeight * 4);
	S.pPoolIn = mmal_port_pool_create (pIn, pIn->buffer_num, pIn->buffer_size);
	S.pPoolOut = mmal_pool_create_with_allocator (pOut->buffer_num, pOut->buffer_size, &S,
						      AllocFrame, FreeFrame);
	if (!S.pPoolIn || !S.pPoolOut)
	{
		Close (nStream);
		return PGPU_ERR_MEMORY;
	}

	pIn->userdata = (MMAL_PORT_USERDATA_T *) &S;
	pOut->userdata = (MMAL_PORT_USERDATA_T *) &S;
	s = mmal_port_enable (pIn, InputCallback);
	CHECK (s, "enable the decoder's input");
	s = mmal_port_enable (pOut, OutputCallback);
	CHECK (s, "enable the ISP's output");
	s = mmal_component_enable (S.pISP);
	CHECK (s, "enable the ISP");
	s = mmal_component_enable (S.pDecoder);
	CHECK (s, "enable the decoder");

	LOGNOTE ("Stream %u: H.264 (%s) %ux%u into texture %u (%ux%u RGBA), %u input buffers",
		 nStream, nFormat == PGPU_VIDEO_AVCC ? "AVCC" : "Annex B", nCodedWidth, nCodedHeight,
		 nTexture, nWidth, nHeight, pIn->buffer_num);
	LOGDBG ("ISP output: %4.4s %ux%u (crop %ux%u), buffers of %u",
		 (const char *) &pOut->format->encoding, pOut->format->es->video.width,
		 pOut->format->es->video.height, pOut->format->es->video.crop.width,
		 pOut->format->es->video.crop.height, pOut->buffer_size);

	return 0;
}

// a chunk of a sample into the ring. A sample is taken whole or not at all:
// its first chunk says how big it is; a rejected sample's other chunks are
// ignored (one ERROR for the sample). A sample's chunks come in order.
u32 CVideo::Data (unsigned nStream, u32 nFlags, s64 nPTS, unsigned nSampleBytes,
		  const u8 *pData, unsigned nBytes)
{
	if (nStream < 1 || nStream > MaxStreams || !m_Streams[nStream].bOpen)
	{
		return PGPU_ERR_OBJECT;
	}
	TStream &S = m_Streams[nStream];

	TSample *pSample;
	if (nFlags & PGPU_VIDEO_FIRST)
	{
		if (S.nSamples && !S.Samples[(S.nFirstSample + S.nSamples - 1) % MaxSamples].bComplete)
		{
			// the last one lost its end: drop it
			S.nSamples--;
			S.nRingIn = S.Samples[(S.nFirstSample + S.nSamples) % MaxSamples].nOffset;
			S.nRingUsed -= S.Samples[(S.nFirstSample + S.nSamples) % MaxSamples].nBytes;
		}
		S.bSkipping = FALSE;
		if (   S.nSamples == MaxSamples || nSampleBytes < nBytes
		    || S.nRingUsed + nSampleBytes > RingBytes)
		{
			S.bSkipping = !(nFlags & PGPU_VIDEO_LAST);
			return PGPU_ERR_LIMIT;			// more than the host was told it could send
		}
		pSample = &S.Samples[(S.nFirstSample + S.nSamples++) % MaxSamples];
		pSample->nOffset = S.nRingIn;
		pSample->nBytes = 0;
		pSample->nSize = nSampleBytes;
		pSample->nSent = 0;
		pSample->nPTS = nPTS;
		pSample->nFlags = nFlags;
		pSample->bComplete = FALSE;
	}
	else
	{
		if (S.bSkipping)
		{
			S.bSkipping = !(nFlags & PGPU_VIDEO_LAST);
			return 0;
		}
		if (!S.nSamples || S.Samples[(S.nFirstSample + S.nSamples - 1) % MaxSamples].bComplete)
		{
			return PGPU_ERR_LIMIT;			// no sample started
		}
		pSample = &S.Samples[(S.nFirstSample + S.nSamples - 1) % MaxSamples];
	}

	if (pSample->nBytes + nBytes > pSample->nSize)
	{
		return PGPU_ERR_LIMIT;				// more than it said (the rest is dropped)
	}
	for (unsigned i = 0; i < nBytes; )
	{
		unsigned n = RingBytes - S.nRingIn;
		n = n < nBytes - i ? n : nBytes - i;
		memcpy (S.pRing + S.nRingIn, pData + i, n);
		S.nRingIn = (S.nRingIn + n) % RingBytes;
		i += n;
	}
	S.nRingUsed += nBytes;
	pSample->nBytes += nBytes;
	if (nFlags & PGPU_VIDEO_LAST)
	{
		pSample->bComplete = TRUE;
		pSample->nFlags |= nFlags & PGPU_VIDEO_EOS;	// (fewer bytes than it said: those)
	}

	return 0;
}

u32 CVideo::Control (unsigned nStream, u32 nOp, s64 nArg)
{
	if (nStream < 1 || nStream > MaxStreams || !m_Streams[nStream].bOpen)
	{
		return PGPU_ERR_OBJECT;
	}
	TStream &S = m_Streams[nStream];
	u64 nNow = CTimer::GetClockTicks64 ();

	switch (nOp)
	{
	case PGPU_VIDEO_PLAY:
		if (nArg == (s64) MMAL_TIME_UNKNOWN)
		{
			nArg = S.nPausedAt != (s64) MMAL_TIME_UNKNOWN ? S.nPausedAt
			       : S.Shown.pBuffer ? S.Shown.nPTS : 0;
		}
		S.nMediaStart = nArg;
		S.nClockStart = nNow;
		S.bPlaying = TRUE;
		S.bStarted = TRUE;
		S.nPausedAt = MMAL_TIME_UNKNOWN;
		break;

	case PGPU_VIDEO_PAUSE:
		S.nPausedAt = MediaTime (S);
		S.bPlaying = FALSE;
		S.bStarted = TRUE;
		break;

	case PGPU_VIDEO_CLOSE:
		Close (nStream);
		break;

	case PGPU_VIDEO_RESIZE:
		return Resize (nStream, (u32) nArg & 0xFFFF, (u32) nArg >> 16);

	default:
		return PGPU_ERR_ENUM;
	}

	return 0;
}

void CVideo::Close (unsigned nStream)
{
	TStream &S = m_Streams[nStream];
	if (!S.bOpen)
	{
		return;
	}

	if (S.pDecoder)
	{
		mmal_component_disable (S.pDecoder);
	}
	if (S.pISP)
	{
		mmal_component_disable (S.pISP);
		if (S.pISP->output[0]->is_enabled)
		{
			mmal_port_disable (S.pISP->output[0]);
		}
	}
	if (S.pDecoder && S.pDecoder->input[0]->is_enabled)
	{
		mmal_port_disable (S.pDecoder->input[0]);
	}
	if (S.pConnection)
	{
		mmal_connection_destroy (S.pConnection);
	}

	for (unsigned i = 0; i < S.nFrames; i++)
	{
		ReleaseFrame (&S.Frames[i]);
	}
	ReleaseFrame (&S.Shown);
	if (S.pDecoded)
	{
		MMAL_BUFFER_HEADER_T *pBuffer;
		while ((pBuffer = mmal_queue_get (S.pDecoded)) != nullptr)
		{
			mmal_buffer_header_release (pBuffer);
		}
		mmal_queue_destroy (S.pDecoded);
	}
	if (S.pPoolIn)
	{
		mmal_port_pool_destroy (S.pDecoder->input[0], S.pPoolIn);
	}
	if (S.pPoolOut)
	{
		mmal_pool_destroy (S.pPoolOut);
	}
	if (S.pISP)
	{
		mmal_component_destroy (S.pISP);
	}
	if (S.pDecoder)
	{
		mmal_component_destroy (S.pDecoder);
	}
	delete [] S.pRing;

	m_pTextures->SetExternal (S.nTexture, nullptr);		// black
	LOGNOTE ("Stream %u closed: %u frames decoded, %u shown, %u dropped", nStream,
		 S.nDecoded, S.nShownFrames, S.nDropped);
	memset (&S, 0, sizeof S);
}

// only the ISP's output changes: its port is disabled (the buffers it has
// come back), the frames are let go, the port gets the new size and a pool
// for it; the decoder, the samples and the clock go on
u32 CVideo::Resize (unsigned nStream, unsigned nWidth, unsigned nHeight)
{
	TStream &S = m_Streams[nStream];
	u32 nError = m_pTextures->CreateExternal (S.nTexture, nWidth, nHeight);	// (checks the size)
	if (nError)
	{
		return nError;
	}

	MMAL_PORT_T *pOut = S.pISP->output[0];
	mmal_port_disable (pOut);
	for (unsigned i = 0; i < S.nFrames; i++)
	{
		ReleaseFrame (&S.Frames[i]);
	}
	S.nFrames = 0;
	ReleaseFrame (&S.Shown);
	MMAL_BUFFER_HEADER_T *pBuffer;
	while ((pBuffer = mmal_queue_get (S.pDecoded)) != nullptr)
	{
		mmal_buffer_header_release (pBuffer);
	}
	mmal_pool_destroy (S.pPoolOut);
	S.pPoolOut = nullptr;

	pOut->format->es->video.width = nWidth;
	pOut->format->es->video.height = nHeight;
	pOut->format->es->video.crop.width = nWidth;
	pOut->format->es->video.crop.height = nHeight;
	MMAL_STATUS_T s = mmal_port_format_commit (pOut);
	if (s == MMAL_SUCCESS)
	{
		pOut->buffer_num = OUTPUT_BUFFERS;
		pOut->buffer_size = pOut->buffer_size_recommended;
		S.pPoolOut = mmal_pool_create_with_allocator (pOut->buffer_num, pOut->buffer_size, &S,
							      AllocFrame, FreeFrame);
		s = S.pPoolOut ? mmal_port_enable (pOut, OutputCallback) : MMAL_ENOMEM;
	}
	if (s != MMAL_SUCCESS)
	{
		LOGWARN ("Stream %u: can't resize to %ux%u (%d)", nStream, nWidth, nHeight, (int) s);
		Close (nStream);
		return PGPU_ERR_MEMORY;
	}
	S.nWidth = nWidth;
	S.nHeight = nHeight;
	LOGNOTE ("Stream %u: texture %u now %ux%u", nStream, S.nTexture, nWidth, nHeight);

	return 0;
}

void CVideo::CloseAll (void)
{
	for (unsigned i = 1; i <= MaxStreams; i++)
	{
		Close (i);
	}
}

// ---- the main loop -------------------------------------------------------------------

void CVideo::Update (void)
{
	for (unsigned i = 1; i <= MaxStreams; i++)
	{
		TStream &S = m_Streams[i];
		if (S.bOpen)
		{
			TakeFrames (S);
			Feed (S);

			u64 nNow = CTimer::GetClockTicks64 ();		// the state, a second (debug)
			if (nNow - S.nLastDebug >= 1000000)
			{
				S.nLastDebug = nNow;
				LOGDBG ("Stream %u: %u samples (%u KB) in the ring, %u KB done; input free %u, "
					 "output free %u, decoded queued %u, waiting %u; decoded %u shown %u "
					 "dropped %u", i, S.nSamples, S.nRingUsed / 1024, S.nBytesDone / 1024,
					 mmal_queue_length (S.pPoolIn->queue), mmal_queue_length (S.pPoolOut->queue),
					 mmal_queue_length (S.pDecoded), S.nFrames, S.nDecoded, S.nShownFrames,
					 S.nDropped);
				LOGDBG ("Stream %u: media %d ms, shown %d ms, waiting %d .. %d ms",
					 i, (int) (MediaTime (S) / 1000), (int) (S.Shown.nPTS / 1000),
					 S.nFrames ? (int) (S.Frames[0].nPTS / 1000) : -1,
					 S.nFrames ? (int) (S.Frames[S.nFrames - 1].nPTS / 1000) : -1);
			}
		}
	}
}

void CVideo::CopyFromRing (TStream &S, unsigned nOffset, u8 *pTo, unsigned nBytes)
{
	nOffset %= RingBytes;
	unsigned n = RingBytes - nOffset;
	n = n < nBytes ? n : nBytes;
	memcpy (pTo, S.pRing + nOffset, n);
	memcpy (pTo + n, S.pRing, nBytes - n);
}

// complete samples to the decoder, in input buffers of up to 64 KB (the first
// with the sample's time and FRAME_START, the last with FRAME_END)
void CVideo::Feed (TStream &S)
{
	while (S.nSamples && !S.bEOSSent)
	{
		TSample &T = S.Samples[S.nFirstSample];
		if (!T.bComplete)
		{
			break;
		}
		MMAL_BUFFER_HEADER_T *pBuffer = mmal_queue_get (S.pPoolIn->queue);
		if (!pBuffer)
		{
			break;
		}

		unsigned n = T.nBytes - T.nSent;
		n = n < pBuffer->alloc_size ? n : pBuffer->alloc_size;
		CopyFromRing (S, T.nOffset + T.nSent, pBuffer->data, n);
		pBuffer->length = n;
		pBuffer->offset = 0;
		pBuffer->flags = 0;
		pBuffer->pts = pBuffer->dts = MMAL_TIME_UNKNOWN;
		if (!T.nSent)
		{
			pBuffer->flags |= MMAL_BUFFER_HEADER_FLAG_FRAME_START;
			pBuffer->pts = T.nPTS;
		}
		T.nSent += n;
		boolean bLast = T.nSent == T.nBytes;
		if (bLast)
		{
			pBuffer->flags |= MMAL_BUFFER_HEADER_FLAG_FRAME_END;
			if (T.nFlags & PGPU_VIDEO_EOS)
			{
				pBuffer->flags |= MMAL_BUFFER_HEADER_FLAG_EOS;
				S.bEOSSent = TRUE;
			}
		}
		if (T.nFlags & PGPU_VIDEO_KEYFRAME)
		{
			pBuffer->flags |= MMAL_BUFFER_HEADER_FLAG_KEYFRAME;
		}
		if (T.nFlags & PGPU_VIDEO_CONFIG)
		{
			pBuffer->flags |= MMAL_BUFFER_HEADER_FLAG_CONFIG;
		}
		if (mmal_port_send_buffer (S.pDecoder->input[0], pBuffer) != MMAL_SUCCESS)
		{
			mmal_buffer_header_release (pBuffer);
			S.bError = TRUE;
			break;
		}

		if (bLast)
		{
			S.nRingUsed -= T.nBytes;
			S.nBytesDone += T.nBytes;
			S.nSamplesDone++;
			S.nFirstSample = (S.nFirstSample + 1) % MaxSamples;
			S.nSamples--;
		}
	}

	// empty frames for the ISP
	MMAL_BUFFER_HEADER_T *pBuffer;
	while ((pBuffer = mmal_queue_get (S.pPoolOut->queue)) != nullptr)
	{
		if (mmal_port_send_buffer (S.pISP->output[0], pBuffer) != MMAL_SUCCESS)
		{
			mmal_buffer_header_release (pBuffer);
			S.bError = TRUE;
			break;
		}
	}
}

// the ISP's frames, in display order, as many as wait for their time (the
// rest stay queued: then the ISP runs out of buffers and waits, and so does
// the decoder)
void CVideo::TakeFrames (TStream &S)
{
	MMAL_BUFFER_HEADER_T *pBuffer;
	while (S.nFrames < MaxFrames && (pBuffer = mmal_queue_get (S.pDecoded)) != nullptr)
	{
		if (pBuffer->flags & MMAL_BUFFER_HEADER_FLAG_EOS)
		{
			S.bEOS = TRUE;
		}
		if (pBuffer->cmd || !pBuffer->length)
		{
			mmal_buffer_header_release (pBuffer);
			continue;
		}

		if (!S.nDecoded)
		{
			const u32 *p = (const u32 *) (pBuffer->data + pBuffer->offset);
			LOGDBG ("First frame: %u bytes at %p (offset %u): %08X %08X %08X %08X",
				 pBuffer->length, pBuffer->data, pBuffer->offset, p[0], p[1], p[2], p[3]);
		}
		S.nDecoded++;
		TFrame &F = S.Frames[S.nFrames++];
		F.pBuffer = pBuffer;
		F.nPTS = pBuffer->pts;

	}
}

s64 CVideo::MediaTime (const TStream &S) const
{
	if (!S.bPlaying)
	{
		return S.nPausedAt;
	}

	return S.nMediaStart + (s64) (CTimer::GetClockTicks64 () - S.nClockStart);
}

void CVideo::ReleaseFrame (TFrame *pFrame)
{
	if (pFrame->pBuffer)
	{
		mmal_buffer_header_release (pFrame->pBuffer);	// back to the pool (Feed sends it)
		pFrame->pBuffer = nullptr;
	}
}

// the GL frame has rendered (so the texture's old frame is free): the newest
// frame that's due goes into the texture, the older due ones are dropped; with
// the clock stopped the first frame is shown (a paused stream shows something)
void CVideo::FrameEnd (void)
{
	for (unsigned i = 1; i <= MaxStreams; i++)
	{
		TStream &S = m_Streams[i];
		if (!S.bOpen || !S.nFrames)
		{
			continue;
		}

		// without PLAY the clock starts when the first frame is shown
		if (!S.bStarted)
		{
			S.bStarted = TRUE;
			S.bPlaying = TRUE;
			S.nMediaStart = S.Frames[0].nPTS != (s64) MMAL_TIME_UNKNOWN ? S.Frames[0].nPTS : 0;
			S.nClockStart = CTimer::GetClockTicks64 ();
		}

		// a gap in the times (samples the host skipped or lost): the clock
		// goes to the next frame, else the video would stop until then
		s64 nNow = MediaTime (S);
		if (   S.bPlaying && nNow != (s64) MMAL_TIME_UNKNOWN
		    && S.Frames[0].nPTS != (s64) MMAL_TIME_UNKNOWN
		    && S.Frames[0].nPTS - nNow > ResyncUS
		    && (!S.Shown.pBuffer || S.Shown.nPTS == (s64) MMAL_TIME_UNKNOWN
			|| S.Frames[0].nPTS - S.Shown.nPTS > ResyncUS))
		{
			LOGNOTE ("Stream %u: a gap in the times, %d ms to %d ms", i,
				 (int) (nNow / 1000), (int) (S.Frames[0].nPTS / 1000));
			S.nMediaStart = S.Frames[0].nPTS;
			S.nClockStart = CTimer::GetClockTicks64 ();
			nNow = S.nMediaStart;
		}

		unsigned nDue = 0;
		if (nNow != (s64) MMAL_TIME_UNKNOWN)
		{
			while (   nDue < S.nFrames
			       && (S.Frames[nDue].nPTS == (s64) MMAL_TIME_UNKNOWN
				   || S.Frames[nDue].nPTS <= nNow))
			{
				nDue++;
			}
		}
		if (!nDue && !S.Shown.pBuffer)
		{
			nDue = 1;
		}
		if (!nDue)
		{
			continue;
		}

		for (unsigned k = 0; k < nDue - 1; k++)
		{
			ReleaseFrame (&S.Frames[k]);
			S.nDropped++;
		}
		ReleaseFrame (&S.Shown);
		S.Shown = S.Frames[nDue - 1];
		S.nFrames -= nDue;
		memmove (S.Frames, S.Frames + nDue, S.nFrames * sizeof S.Frames[0]);
		S.nShownFrames++;

		m_pTextures->SetExternal (S.nTexture, S.Shown.pBuffer->data + S.Shown.pBuffer->offset);
	}
}

boolean CVideo::GetStatus (unsigned nStream, u32 *pPayload, boolean bDue)
{
	if (nStream < 1 || nStream > MaxStreams)
	{
		return FALSE;
	}
	TStream &S = m_Streams[nStream];
	u64 nNow = CTimer::GetClockTicks64 ();
	if (bDue && (!S.bOpen || nNow - S.nLastStatus < STATUS_US))
	{
		return FALSE;
	}
	S.nLastStatus = nNow;

	s64 nShownPTS = S.Shown.pBuffer ? S.Shown.nPTS : (s64) MMAL_TIME_UNKNOWN;
	pPayload[0] = nStream;
	pPayload[1] =   (S.bOpen ? PGPU_VIDEO_OPEN_FLAG : 0)
		      | (S.bPlaying ? PGPU_VIDEO_PLAYING : 0)
		      | (S.bEOS ? PGPU_VIDEO_ENDED : 0)
		      | (S.bError ? PGPU_VIDEO_ERROR : 0);
	pPayload[2] = S.nBytesDone;
	pPayload[3] = RingBytes;
	pPayload[4] = S.nDecoded;
	pPayload[5] = S.nShownFrames;
	pPayload[6] = S.nDropped;
	pPayload[7] = (u32) nShownPTS;
	pPayload[8] = (u32) ((u64) nShownPTS >> 32);
	pPayload[9] = S.nFrames;
	pPayload[10] = S.nSamplesDone;
	pPayload[11] = MaxSamples;

	return TRUE;
}

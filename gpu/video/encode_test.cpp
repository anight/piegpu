//
// encode_test.cpp
//
#include "encode_test.h"
#include "../audio/audio_sink.h"
#include <circle/logger.h>
#include <circle/string.h>
#include <circle/timer.h>
#include <circle/util.h>
#include <assert.h>

extern "C" {
#include "interface/mmal/mmal.h"
#include "interface/mmal/util/mmal_util.h"
#include "interface/mmal/util/mmal_util_params.h"
}

LOGMODULE ("enc");

CEncodeTest::CEncodeTest (void)
:	m_bRunning (FALSE),
	m_pEncoder (nullptr),
	m_pPoolIn (nullptr),
	m_pPoolOut (nullptr),
	m_pKeep (nullptr),
	m_nKeepBytes (0),
	m_nKept (0),
	m_nTookUs (0),
	m_pSound (nullptr),
	m_nSoundFrames (0),
	m_nSoundRate (0),
	m_bTapped (FALSE),
	m_pFrame (nullptr),
	m_nFrameMax (0)
{
}

CEncodeTest::~CEncodeTest (void)
{
	Stop ();
	delete [] m_pKeep;
	delete [] m_pSound;
	delete [] m_pFrame;
}

// ---- MMAL's callbacks (VCHIQ's tasks: between the main loop's rounds) -------------------

void CEncodeTest::ControlCallback (MMAL_PORT_T *pPort, MMAL_BUFFER_HEADER_T *pBuffer)
{
	CEncodeTest *pThis = (CEncodeTest *) pPort->userdata;
	if (pBuffer->cmd == MMAL_EVENT_ERROR)
	{
		pThis->m_bError = TRUE;
		LOGWARN ("%s: error %d", pPort->component->name, *(int *) pBuffer->data);
	}
	mmal_buffer_header_release (pBuffer);
}

void CEncodeTest::InputCallback (MMAL_PORT_T *pPort, MMAL_BUFFER_HEADER_T *pBuffer)
{
	mmal_buffer_header_release (pBuffer);		// back to the input pool
}

// a piece of the stream: counted, kept while there's room, and its buffer
// given back to the encoder
void CEncodeTest::OutputCallback (MMAL_PORT_T *pPort, MMAL_BUFFER_HEADER_T *pBuffer)
{
	CEncodeTest *pThis = (CEncodeTest *) pPort->userdata;
	if (!pBuffer->cmd && pBuffer->length)
	{
		TCounts &C = pThis->m_Second;
		C.nBytes += pBuffer->length;
		if (pThis->m_File.IsOpen ())			// the file's: parameter sets, or a piece of a frame
		{
			const u8 *pData = pBuffer->data + pBuffer->offset;
			if (pBuffer->flags & MMAL_BUFFER_HEADER_FLAG_CONFIG)
			{
				pThis->m_File.Config (pData, pBuffer->length);
			}
			else if (pThis->m_nFrameFill + pBuffer->length <= pThis->m_nFrameMax)
			{
				memcpy (pThis->m_pFrame + pThis->m_nFrameFill, pData, pBuffer->length);
				pThis->m_nFrameFill += pBuffer->length;
				if (pBuffer->flags & MMAL_BUFFER_HEADER_FLAG_FRAME_END)
				{
					pThis->m_File.Video (pThis->m_pFrame, pThis->m_nFrameFill,
							     !!(pBuffer->flags & MMAL_BUFFER_HEADER_FLAG_KEYFRAME),
							     pBuffer->pts != MMAL_TIME_UNKNOWN ? (u64) pBuffer->pts
											       : CTimer::GetClockTicks64 ());
					pThis->m_nFrameFill = 0;
				}
			}
		}
		pThis->m_nFrameBytes += pBuffer->length;
		if (pThis->m_nKept + pBuffer->length <= pThis->m_nKeepBytes)
		{
			memcpy (pThis->m_pKeep + pThis->m_nKept, pBuffer->data + pBuffer->offset, pBuffer->length);
			pThis->m_nKept += pBuffer->length;
		}
		else
		{
			pThis->m_nKeepBytes = pThis->m_nKept;	// (no later piece after a gap)
		}
		if (   (pBuffer->flags & MMAL_BUFFER_HEADER_FLAG_FRAME_END)
		    && !(pBuffer->flags & MMAL_BUFFER_HEADER_FLAG_CONFIG))
		{
			boolean bKey = !!(pBuffer->flags & MMAL_BUFFER_HEADER_FLAG_KEYFRAME);
			C.nFrames++;
			C.nKeyFrames += bKey;
			unsigned &rBiggest = bKey ? C.nBiggestKey : C.nBiggest;
			rBiggest = pThis->m_nFrameBytes > rBiggest ? pThis->m_nFrameBytes : rBiggest;
			pThis->m_nFrameBytes = 0;
			if (pBuffer->pts != MMAL_TIME_UNKNOWN)
			{
				unsigned nLate = (unsigned) (CTimer::GetClockTicks64 () - (u64) pBuffer->pts);
				C.nLateUs += nLate;
				C.nLatestUs = nLate > C.nLatestUs ? nLate : C.nLatestUs;
			}
		}
	}
	mmal_buffer_header_release (pBuffer);

	if (pPort->is_enabled && pThis->m_pPoolOut)
	{
		MMAL_BUFFER_HEADER_T *pNext = mmal_queue_get (pThis->m_pPoolOut->queue);
		if (pNext && mmal_port_send_buffer (pPort, pNext) != MMAL_SUCCESS)
		{
			mmal_buffer_header_release (pNext);
			pThis->m_bError = TRUE;
		}
	}
}

// ---- the test -----------------------------------------------------------------------

#define CHECK(s, what)							\
	if ((s) != MMAL_SUCCESS)					\
	{								\
		LOGWARN ("%s: status %d", what, (int) (s));		\
		Stop ();						\
		return FALSE;						\
	}
#define TRY(s, what)							\
	{								\
		MMAL_STATUS_T nTried = (s);				\
		if (nTried != MMAL_SUCCESS)				\
		{							\
			LOGWARN ("%s: status %d (goes on without)", what, (int) nTried);	\
		}							\
	}

boolean CEncodeTest::Start (unsigned nWidth, unsigned nHeight, unsigned nFrameRate, unsigned nBitrate,
			    unsigned nSeconds, boolean bI420, unsigned nKeepBytes, const char *pFile)
{
	Stop ();
	if (m_File.IsOpen ())			// (one that didn't end)
	{
		m_File.Close ();
	}
	m_nFrameFill = 0;
	m_nLastFileWork = 0;
	if (!m_pFrame)
	{
		m_nFrameMax = 512 * 1024;
		m_pFrame = new u8[m_nFrameMax];
	}
	delete [] m_pKeep;
	m_pKeep = nKeepBytes ? new u8[nKeepBytes] : nullptr;
	m_nKeepBytes = m_pKeep ? nKeepBytes : 0;
	m_nKept = 0;
	delete [] m_pSound;
	m_nSoundMax = (nSeconds + 1) * 48000;		// (the outputs' rates: 48000 at most)
	m_pSound = nKeepBytes ? new s16[m_nSoundMax * 2] : nullptr;
	m_nSoundFrames = m_nSoundRate = 0;
	m_bSoundStarted = FALSE;
	m_nFirstFrameUs = 0;
	m_bI420 = bI420;
	m_bError = FALSE;
	m_nWidth = nWidth;
	m_nHeight = nHeight;
	m_nStride = (nWidth + 31) & ~31u;		// the encoder's rows and their number: rounded up
	m_nRows = (nHeight + 15) & ~15u;
	m_nFrameBytes = 0;
	memset (&m_Second, 0, sizeof m_Second);
	memset (&m_Total, 0, sizeof m_Total);

	MMAL_STATUS_T s = mmal_component_create ("vc.ril.video_encode", &m_pEncoder);
	CHECK (s, "create vc.ril.video_encode");
	m_pEncoder->control->userdata = (MMAL_PORT_USERDATA_T *) this;
	mmal_port_enable (m_pEncoder->control, ControlCallback);
	MMAL_PORT_T *pIn = m_pEncoder->input[0], *pOut = m_pEncoder->output[0];

	// what its input takes
	struct
	{
		MMAL_PARAMETER_HEADER_T Header;
		MMAL_FOURCC_T Encodings[40];
	}
	Takes;
	memset (&Takes, 0, sizeof Takes);
	Takes.Header.id = MMAL_PARAMETER_SUPPORTED_ENCODINGS;
	Takes.Header.size = sizeof Takes;
	if (mmal_port_parameter_get (pIn, &Takes.Header) == MMAL_SUCCESS)
	{
		CString List;
		unsigned n = (Takes.Header.size - sizeof Takes.Header) / sizeof (MMAL_FOURCC_T);
		for (unsigned i = 0; i < n && i < 40; i++)
		{
			char Name[6] = {' ', 0, 0, 0, 0, 0};
			memcpy (Name + 1, &Takes.Encodings[i], 4);
			List.Append (Name);
		}
		LOGNOTE ("The encoder's input takes:%s", (const char *) List);
	}

	MMAL_ES_FORMAT_T *pFormat = pIn->format;
	pFormat->type = MMAL_ES_TYPE_VIDEO;
	pFormat->encoding = bI420 ? MMAL_ENCODING_I420 : MMAL_ENCODING_RGB16;
	pFormat->es->video.width = m_nStride;
	pFormat->es->video.height = m_nRows;
	pFormat->es->video.crop.x = 0;
	pFormat->es->video.crop.y = 0;
	pFormat->es->video.crop.width = nWidth;
	pFormat->es->video.crop.height = nHeight;
	pFormat->es->video.frame_rate.num = nFrameRate;
	pFormat->es->video.frame_rate.den = 1;
	pFormat->es->video.par.num = 1;
	pFormat->es->video.par.den = 1;
	s = mmal_port_format_commit (pIn);
	CHECK (s, "encoder input format");

	mmal_format_copy (pOut->format, pIn->format);
	pOut->format->encoding = MMAL_ENCODING_H264;
	pOut->format->bitrate = nBitrate;
	pOut->format->es->video.frame_rate.num = 0;	// (as raspivid: the stream has no rate of its own)
	pOut->format->es->video.frame_rate.den = 1;
	s = mmal_port_format_commit (pOut);
	CHECK (s, "encoder output format");

	MMAL_PARAMETER_VIDEO_PROFILE_T Profile;
	memset (&Profile, 0, sizeof Profile);
	Profile.hdr.id = MMAL_PARAMETER_PROFILE;
	Profile.hdr.size = sizeof Profile;
	Profile.profile[0].profile = MMAL_VIDEO_PROFILE_H264_HIGH;
	Profile.profile[0].level = MMAL_VIDEO_LEVEL_H264_4;
	TRY (mmal_port_parameter_set (pOut, &Profile.hdr), "high profile, level 4");
	TRY (mmal_port_parameter_set_uint32 (pOut, MMAL_PARAMETER_INTRAPERIOD, nFrameRate), "a key frame a second");
	TRY (mmal_port_parameter_set_boolean (pOut, MMAL_PARAMETER_VIDEO_ENCODE_INLINE_HEADER, MMAL_TRUE),
	     "SPS and PPS with every key frame");
	TRY (mmal_port_parameter_set_boolean (pIn, MMAL_PARAMETER_VIDEO_IMMUTABLE_INPUT, MMAL_TRUE), "immutable input");

	unsigned nFrameIn = bI420 ? m_nStride * m_nRows * 3 / 2 : m_nStride * m_nRows * 2;
	pIn->buffer_num = pIn->buffer_num_recommended > 3 ? pIn->buffer_num_recommended : 3;
	pIn->buffer_size = pIn->buffer_size_recommended > nFrameIn ? pIn->buffer_size_recommended : nFrameIn;
	pOut->buffer_num = pOut->buffer_num_recommended > 3 ? pOut->buffer_num_recommended : 3;
	pOut->buffer_size = pOut->buffer_size_recommended;
	m_pPoolIn = mmal_port_pool_create (pIn, pIn->buffer_num, pIn->buffer_size);
	m_pPoolOut = mmal_port_pool_create (pOut, pOut->buffer_num, pOut->buffer_size);
	if (!m_pPoolIn || !m_pPoolOut)
	{
		LOGWARN ("No buffers");
		Stop ();
		return FALSE;
	}

	pIn->userdata = (MMAL_PORT_USERDATA_T *) this;
	pOut->userdata = (MMAL_PORT_USERDATA_T *) this;
	s = mmal_port_enable (pIn, InputCallback);
	CHECK (s, "enable the encoder's input");
	s = mmal_port_enable (pOut, OutputCallback);
	CHECK (s, "enable the encoder's output");
	s = mmal_component_enable (m_pEncoder);
	CHECK (s, "enable the encoder");
	MMAL_BUFFER_HEADER_T *pBuffer;
	while ((pBuffer = mmal_queue_get (m_pPoolOut->queue)) != nullptr)
	{
		s = mmal_port_send_buffer (pOut, pBuffer);
		CHECK (s, "a buffer for the stream");
	}

	LOGNOTE ("H.264 of %ux%u (the encoder's %ux%u, %s), %u frames a second expected, %u kbit/s, for %u s; "
		 "input: %u buffers of %u, output: %u of %u", nWidth, nHeight, m_nStride, m_nRows,
		 bI420 ? "I420 made here" : "RGB16 as it is", nFrameRate, nBitrate / 1000, nSeconds,
		 pIn->buffer_num, pIn->buffer_size, pOut->buffer_num, pOut->buffer_size);

	if (pFile && !m_File.Open (pFile, nWidth, nHeight))
	{
		Stop ();
		return FALSE;
	}
	m_nStart = m_nLastReport = CTimer::GetClockTicks64 ();
	m_nEnd = m_nStart + (u64) nSeconds * 1000000;
	m_bRunning = TRUE;

	return TRUE;
}

void CEncodeTest::Stop (void)
{
	m_bRunning = FALSE;
	if (m_bTapped)
	{
		CAudioSink::StopTap ();
		TakeSound ();
		m_bTapped = FALSE;
	}
	if (m_pEncoder)
	{
		mmal_port_disable (m_pEncoder->input[0]);
		mmal_port_disable (m_pEncoder->output[0]);
		mmal_component_disable (m_pEncoder);
		mmal_port_disable (m_pEncoder->control);
	}
	if (m_pPoolIn)
	{
		mmal_port_pool_destroy (m_pEncoder->input[0], m_pPoolIn);
		m_pPoolIn = nullptr;
	}
	if (m_pPoolOut)
	{
		MMAL_POOL_T *pPool = m_pPoolOut;
		m_pPoolOut = nullptr;
		mmal_port_pool_destroy (m_pEncoder->output[0], pPool);
	}
	if (m_pEncoder)
	{
		mmal_component_destroy (m_pEncoder);
		m_pEncoder = nullptr;
	}
}

// RGB565 to the encoder's buffer: as it is, or as I420 (BT.601, limited
// range; a chroma sample from the top left pixel of its four)
void CEncodeTest::Frame (const u16 *pRGB565)
{
	if (!m_bRunning)
	{
		return;
	}
	TCounts &C = m_Second;
	C.nOffered++;
	MMAL_BUFFER_HEADER_T *pBuffer = mmal_queue_get (m_pPoolIn->queue);
	if (!pBuffer)
	{
		C.nNoBuffer++;
		return;
	}
	u64 nNow = CTimer::GetClockTicks64 ();
	if (!m_bTapped && (m_pSound || m_File.IsOpen ()))	// the sound from the first frame on
	{
		m_nFirstFrameUs = nNow;
		CAudioSink::StartTap ();
		m_bTapped = TRUE;
	}

	if (!m_bI420)
	{
		for (unsigned y = 0; y < m_nHeight; y++)
		{
			memcpy (pBuffer->data + y * m_nStride * 2, pRGB565 + y * m_nWidth, m_nWidth * 2);
		}
		pBuffer->length = m_nStride * m_nRows * 2;
	}
	else
	{
		u8 *pY = pBuffer->data, *pU = pY + m_nStride * m_nRows, *pV = pU + m_nStride * m_nRows / 4;
		for (unsigned y = 0; y < m_nHeight; y++)
		{
			const u16 *pFrom = pRGB565 + y * m_nWidth;
			u8 *pTo = pY + y * m_nStride;
			boolean bChroma = !(y & 1);
			u8 *pToU = pU + (y / 2) * (m_nStride / 2), *pToV = pV + (y / 2) * (m_nStride / 2);
			for (unsigned x = 0; x < m_nWidth; x++)
			{
				unsigned p = pFrom[x];
				int r = (p >> 11) * 255 / 31, g = ((p >> 5) & 63) * 255 / 63, b = (p & 31) * 255 / 31;
				pTo[x] = (u8) ((66 * r + 129 * g + 25 * b + 128) / 256 + 16);
				if (bChroma && !(x & 1))
				{
					pToU[x / 2] = (u8) ((-38 * r - 74 * g + 112 * b + 128) / 256 + 128);
					pToV[x / 2] = (u8) ((112 * r - 94 * g - 18 * b + 128) / 256 + 128);
				}
			}
		}
		pBuffer->length = m_nStride * m_nRows * 3 / 2;
	}
	pBuffer->offset = 0;
	pBuffer->flags = MMAL_BUFFER_HEADER_FLAG_FRAME_START | MMAL_BUFFER_HEADER_FLAG_FRAME_END;
	pBuffer->pts = (s64) nNow;
	pBuffer->dts = MMAL_TIME_UNKNOWN;
	if (mmal_port_send_buffer (m_pEncoder->input[0], pBuffer) != MMAL_SUCCESS)
	{
		mmal_buffer_header_release (pBuffer);
		m_bError = TRUE;
		return;
	}
	C.nSent++;
	C.nGivingUs += (unsigned) (CTimer::GetClockTicks64 () - nNow);
}

void CEncodeTest::FrameSlack (void)
{
	if (m_bRunning)
	{
		m_File.Work ();
		m_nLastFileWork = CTimer::GetClockTicks64 ();
	}
}

void CEncodeTest::Report (const char *pWhat, unsigned nUs)
{
	const TCounts &C = *(strcmp (pWhat, "all") == 0 ? &m_Total : &m_Second);
	unsigned nTenths = nUs ? (unsigned) ((u64) C.nFrames * 10000000 / nUs) : 0;
	LOGNOTE ("%s: in %u of %u frames (%u without a buffer), %u us each to give; out %u.%u frames a second "
		 "(%u key), %u kbit/s, a frame at most %u bytes (a key frame %u); late %u ms, at most %u ms",
		 pWhat, C.nSent, C.nOffered, C.nNoBuffer, C.nSent ? C.nGivingUs / C.nSent : 0,
		 nTenths / 10, nTenths % 10, C.nKeyFrames,
		 nUs ? (unsigned) ((u64) C.nBytes * 8000 / nUs) : 0, C.nBiggest, C.nBiggestKey,
		 C.nFrames ? (unsigned) (C.nLateUs / C.nFrames / 1000) : 0, C.nLatestUs / 1000);
}

// the sound taken since the last time: kept, and to the file. Its first
// frames were taken a little after the first frame was given: silence for as long
void CEncodeTest::TakeSound (void)
{
	static s16 Frames[4800 * 2];
	unsigned n, nRate = CAudioSink::GetTapRate ();
	if (!m_bTapped || !nRate)
	{
		return;
	}
	if (!m_bSoundStarted)
	{
		m_bSoundStarted = TRUE;
		m_nSoundRate = nRate;
		u64 nStart = CAudioSink::GetTapStart ();
		unsigned nLateUs = nStart > m_nFirstFrameUs ? (unsigned) (nStart - m_nFirstFrameUs) : 0;
		unsigned nSilent = (unsigned) ((u64) (nLateUs < 100000 ? nLateUs : 100000) * nRate / 1000000);
		memset (Frames, 0, nSilent * 4);
		if (m_pSound)
		{
			memcpy (m_pSound, Frames, nSilent * 4);
			m_nSoundFrames = nSilent;
		}
		m_File.Sound (Frames, nSilent, nRate);
	}
	while ((n = CAudioSink::ReadTap (Frames, 4800)) != 0)
	{
		if (m_pSound)
		{
			unsigned nKeep = m_nSoundMax - m_nSoundFrames < n ? m_nSoundMax - m_nSoundFrames : n;
			memcpy (m_pSound + 2 * m_nSoundFrames, Frames, nKeep * 4);
			m_nSoundFrames += nKeep;
		}
		m_File.Sound (Frames, n, nRate);
	}
}

boolean CEncodeTest::Update (void)
{
	if (!m_bRunning)
	{
		return FALSE;
	}
	TakeSound ();
	if (CTimer::GetClockTicks64 () - m_nLastFileWork > 50000)	// (no frames shown: here then)
	{
		FrameSlack ();
	}
	if (m_File.IsOpen () && !m_File.IsGood ())
	{
		m_bError = TRUE;
	}
	u64 nNow = CTimer::GetClockTicks64 ();
	if (nNow - m_nLastReport >= 1000000 || nNow >= m_nEnd || m_bError)
	{
		Report ("a second", (unsigned) (nNow - m_nLastReport));
		m_nLastReport = nNow;
		const TCounts &S = m_Second;
		TCounts &T = m_Total;
		T.nOffered += S.nOffered;
		T.nSent += S.nSent;
		T.nNoBuffer += S.nNoBuffer;
		T.nFrames += S.nFrames;
		T.nKeyFrames += S.nKeyFrames;
		T.nBytes += S.nBytes;
		T.nBiggest = S.nBiggest > T.nBiggest ? S.nBiggest : T.nBiggest;
		T.nBiggestKey = S.nBiggestKey > T.nBiggestKey ? S.nBiggestKey : T.nBiggestKey;
		T.nGivingUs += S.nGivingUs;
		T.nLateUs += S.nLateUs;
		T.nLatestUs = S.nLatestUs > T.nLatestUs ? S.nLatestUs : T.nLatestUs;
		memset (&m_Second, 0, sizeof m_Second);
	}
	if (nNow < m_nEnd && !m_bError)
	{
		return FALSE;
	}

	m_nTookUs = (unsigned) (nNow - m_nStart);
	Report ("all", m_nTookUs);
	if (m_bError)
	{
		LOGWARN ("Ended by an error");
	}
	Stop ();
	if (m_pSound)
	{
		LOGNOTE ("The sound meanwhile: %u frames at %u Hz (%u ms)", m_nSoundFrames, m_nSoundRate,
			 m_nSoundRate ? (unsigned) ((u64) m_nSoundFrames * 1000 / m_nSoundRate) : 0);
	}
	if (m_File.IsOpen ())
	{
		unsigned nWrites, nLongestUs, nMostWaiting, nFrames = m_File.GetVideoFrames ();
		u64 nWriteUs;
		unsigned nCloseStart = CTimer::GetClockTicks ();
		boolean bGood = m_File.Close ();
		unsigned nCloseUs = CTimer::GetClockTicks () - nCloseStart;
		m_File.GetWrites (&nWrites, &nWriteUs, &nLongestUs, &nMostWaiting);
		LOGNOTE ("The file: %s, %u KB, %u frames, %u lost frames of sound; the card: %u writes, %u ms each, "
			 "at most %u ms, %u KB waiting at most; closing it %u ms", bGood ? "good" : "NO GOOD",
			 (unsigned) (m_File.GetBytes () / 1024), nFrames, CAudioSink::GetTapLost (), nWrites,
			 nWrites ? (unsigned) (nWriteUs / nWrites / 1000) : 0, nLongestUs / 1000, nMostWaiting / 1024,
			 nCloseUs / 1000);
	}

	return TRUE;
}

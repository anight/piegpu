//
// videodec: H.264 on the VideoCore's decoder from bare metal - MMAL (the
// userland's client, BSD) over Circle's VCHIQ. Decodes the linked-in test
// stream (720p, High, B-frames) to I420 frames on the ARM side as fast as
// it goes, counts them and dumps frame 100's luma, downscaled, to the log.
//
#include "kernel.h"
#include <circle/string.h>
#include <circle/util.h>

extern "C" {
#include "interface/mmal/mmal.h"
#include "interface/mmal/util/mmal_util.h"
#include "interface/mmal/util/mmal_util_params.h"
#include "interface/mmal/vc/mmal_vc_api.h"

extern const u8 test_stream[], test_stream_end[];

unsigned long long circle_micros (void)
{
	return CTimer::GetClockTicks64 ();
}
}

#define CHUNK		(64 * 1024)
#define DUMP_FRAME	100
#define DUMP_STEP	8

LOGMODULE ("videodec");

static MMAL_QUEUE_T *s_pOutQueue;

static void ControlCallback (MMAL_PORT_T *pPort, MMAL_BUFFER_HEADER_T *pBuffer)
{
	if (pBuffer->cmd == MMAL_EVENT_ERROR)
	{
		LOGERR ("decoder error %d", *(MMAL_STATUS_T *) pBuffer->data);
	}
	mmal_buffer_header_release (pBuffer);
}

static void InputCallback (MMAL_PORT_T *pPort, MMAL_BUFFER_HEADER_T *pBuffer)
{
	mmal_buffer_header_release (pBuffer);
}

static void OutputCallback (MMAL_PORT_T *pPort, MMAL_BUFFER_HEADER_T *pBuffer)
{
	mmal_queue_put (s_pOutQueue, pBuffer);
}

CKernel::CKernel (void)
:	m_Timer (&m_Interrupt),
	m_Logger (m_Options.GetLogLevel (), &m_Timer),
	m_DevLink (&m_Interrupt),
	m_VCHIQ (CMemorySystem::Get (), &m_Interrupt)
{
}

CKernel::~CKernel (void)
{
}

boolean CKernel::Initialize (void)
{
	return    m_Logger.Initialize (&m_Null)
	       && m_Interrupt.Initialize ()
	       && m_Timer.Initialize ()
	       && m_DevLink.Initialize ()
	       && m_VCHIQ.Initialize ();
}

TShutdownMode CKernel::Run (void)
{
	LOGNOTE ("videodec built " __DATE__ " " __TIME__ ", stream %u bytes",
		 (unsigned) (test_stream_end - test_stream));

	Decode ();

	while (1)
	{
		m_DevLink.Update ();
		m_Scheduler.MsSleep (10);
	}

	return ShutdownHalt;
}

#define CHECK(s, what)	if ((s) != MMAL_SUCCESS) { LOGERR ("%s: status %d", what, (int) (s)); return; }

void CKernel::Decode (void)
{
	MMAL_STATUS_T s = mmal_vc_init ();
	CHECK (s, "mmal_vc_init");
	LOGNOTE ("MMAL connected to the VideoCore");

	MMAL_COMPONENT_T *pDecoder;
	s = mmal_component_create ("vc.ril.video_decode", &pDecoder);
	CHECK (s, "create vc.ril.video_decode");
	s = mmal_port_enable (pDecoder->control, ControlCallback);
	CHECK (s, "enable control port");

	MMAL_PORT_T *pIn = pDecoder->input[0], *pOut = pDecoder->output[0];
	MMAL_ES_FORMAT_T *pFormat = pIn->format;
	pFormat->type = MMAL_ES_TYPE_VIDEO;
	pFormat->encoding = MMAL_ENCODING_H264;
	pFormat->es->video.width = 1280;
	pFormat->es->video.height = 720;
	pFormat->es->video.frame_rate.num = 30;
	pFormat->es->video.frame_rate.den = 1;
	pFormat->es->video.par.num = 1;
	pFormat->es->video.par.den = 1;
	s = mmal_port_format_commit (pIn);
	CHECK (s, "input format");

	pOut->format->encoding = MMAL_ENCODING_I420;
	s = mmal_port_format_commit (pOut);
	CHECK (s, "output format");

	pIn->buffer_num = pIn->buffer_num_recommended;
	pIn->buffer_size = CHUNK;
	pOut->buffer_num = pOut->buffer_num_recommended;
	pOut->buffer_size = pOut->buffer_size_recommended;
	LOGNOTE ("input: %u buffers of %u; output %ux%u: %u buffers of %u",
		 pIn->buffer_num, pIn->buffer_size, pOut->format->es->video.width,
		 pOut->format->es->video.height, pOut->buffer_num, pOut->buffer_size);

	MMAL_POOL_T *pPoolIn = mmal_port_pool_create (pIn, pIn->buffer_num, pIn->buffer_size);
	MMAL_POOL_T *pPoolOut = mmal_port_pool_create (pOut, pOut->buffer_num, pOut->buffer_size);
	s_pOutQueue = mmal_queue_create ();
	if (!pPoolIn || !pPoolOut || !s_pOutQueue)
	{
		LOGERR ("no pools");
		return;
	}

	s = mmal_port_enable (pIn, InputCallback);
	CHECK (s, "enable input");
	s = mmal_port_enable (pOut, OutputCallback);
	CHECK (s, "enable output");
	s = mmal_component_enable (pDecoder);
	CHECK (s, "enable decoder");

	const u8 *pData = test_stream;
	boolean bEOSSent = FALSE, bEOS = FALSE;
	unsigned nFrames = 0, nFormatChanges = 0;
	u64 nStart = CTimer::GetClockTicks64 (), nFirst = 0, nLastLog = nStart;
	while (!bEOS)
	{
		m_DevLink.Update ();

		MMAL_BUFFER_HEADER_T *pBuffer;
		while ((pBuffer = mmal_queue_get (s_pOutQueue)) != nullptr)
		{
			if (pBuffer->cmd == MMAL_EVENT_FORMAT_CHANGED)
			{
				MMAL_EVENT_FORMAT_CHANGED_T *pEvent = mmal_event_format_changed_get (pBuffer);
				nFormatChanges++;
				LOGNOTE ("format changed: %ux%u (crop %ux%u), %u buffers of %u",
					 pEvent->format->es->video.width, pEvent->format->es->video.height,
					 pEvent->format->es->video.crop.width, pEvent->format->es->video.crop.height,
					 pEvent->buffer_num_recommended, pEvent->buffer_size_recommended);
				mmal_port_disable (pOut);
				mmal_format_full_copy (pOut->format, pEvent->format);
				pOut->buffer_num = pEvent->buffer_num_recommended;
				pOut->buffer_size = pEvent->buffer_size_recommended;
				mmal_buffer_header_release (pBuffer);
				s = mmal_port_format_commit (pOut);
				CHECK (s, "output format (changed)");
				mmal_pool_destroy (pPoolOut);
				pPoolOut = mmal_port_pool_create (pOut, pOut->buffer_num, pOut->buffer_size);
				s = mmal_port_enable (pOut, OutputCallback);
				CHECK (s, "enable output (changed)");
				continue;
			}

			if (pBuffer->length)
			{
				if (!nFrames++)
				{
					nFirst = CTimer::GetClockTicks64 ();
				}
				if (nFrames == DUMP_FRAME)
				{
					const MMAL_VIDEO_FORMAT_T &V = pOut->format->es->video;
					mmal_buffer_header_mem_lock (pBuffer);
					DumpLuma (pBuffer->data + pBuffer->offset, V.width, V.crop.width,
						  V.crop.height);
					mmal_buffer_header_mem_unlock (pBuffer);
				}
			}
			if (pBuffer->flags & MMAL_BUFFER_HEADER_FLAG_EOS)
			{
				bEOS = TRUE;
			}
			mmal_buffer_header_release (pBuffer);
		}

		while ((pBuffer = mmal_queue_get (pPoolOut->queue)) != nullptr)
		{
			s = mmal_port_send_buffer (pOut, pBuffer);
			CHECK (s, "send output buffer");
		}

		while (!bEOSSent && (pBuffer = mmal_queue_get (pPoolIn->queue)) != nullptr)
		{
			unsigned nLength = test_stream_end - pData;
			if (nLength > pBuffer->alloc_size)
			{
				nLength = pBuffer->alloc_size;
			}
			memcpy (pBuffer->data, pData, nLength);
			pData += nLength;
			pBuffer->length = nLength;
			pBuffer->offset = 0;
			pBuffer->pts = pBuffer->dts = MMAL_TIME_UNKNOWN;
			pBuffer->flags = 0;
			if (pData == test_stream_end)
			{
				pBuffer->flags = MMAL_BUFFER_HEADER_FLAG_EOS;
				bEOSSent = TRUE;
			}
			s = mmal_port_send_buffer (pIn, pBuffer);
			CHECK (s, "send input buffer");
		}

		u64 nNow = CTimer::GetClockTicks64 ();
		if (nNow - nLastLog > 1000000)
		{
			LOGNOTE ("%u frames, %u KB of %u sent", nFrames,
				 (unsigned) ((pData - test_stream) / 1024),
				 (unsigned) ((test_stream_end - test_stream) / 1024));
			nLastLog = nNow;
			if (nNow - nStart > 60000000)
			{
				LOGERR ("timeout");
				break;
			}
		}

		m_Scheduler.Yield ();
	}

	u64 nEnd = CTimer::GetClockTicks64 ();
	unsigned nUs = (unsigned) (nEnd - nFirst);
	LOGNOTE ("done: %u frames (%u format changes) in %u ms after the first, %u.%u fps; "
		 "first frame after %u ms", nFrames, nFormatChanges, nUs / 1000,
		 nFrames && nUs ? (unsigned) ((u64) nFrames * 1000000 / nUs) : 0,
		 nFrames && nUs ? (unsigned) ((u64) nFrames * 10000000 / nUs % 10) : 0,
		 (unsigned) ((nFirst - nStart) / 1000));

	mmal_component_disable (pDecoder);
}

// frame DUMP_FRAME's luma, every DUMP_STEP'th pixel, as hex lines between markers
void CKernel::DumpLuma (const u8 *pY, unsigned nStride, unsigned nWidth, unsigned nHeight)
{
	unsigned w = nWidth / DUMP_STEP, h = nHeight / DUMP_STEP;
	u32 nSum = 0;
	for (unsigned y = 0; y < nHeight; y++)
	{
		for (unsigned x = 0; x < nWidth; x++)
		{
			nSum += pY[y * nStride + x];
		}
	}
	LOGNOTE ("frame %u: %ux%u, stride %u, luma sum %u", DUMP_FRAME, nWidth, nHeight, nStride, nSum);

	CString Line;
	Line.Format ("\n#LUMA %u %u\n", w, h);
	m_DevLink.Write ((const char *) Line, Line.GetLength ());
	static const char Hex[] = "0123456789abcdef";
	char Buf[2 * 256 + 2];
	for (unsigned y = 0; y < h; y++)
	{
		unsigned n = 0;
		for (unsigned x = 0; x < w; x++)
		{
			u8 v = pY[y * DUMP_STEP * nStride + x * DUMP_STEP];
			Buf[n++] = Hex[v >> 4];
			Buf[n++] = Hex[v & 15];
		}
		Buf[n++] = '\n';
		m_DevLink.Write (Buf, n);
		m_DevLink.Update ();
	}
	m_DevLink.Write ("#END\n", 5);
}

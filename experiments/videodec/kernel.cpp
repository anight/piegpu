//
// videodec: H.264 on the VideoCore's decoder from bare metal - MMAL (the
// userland's client, BSD) over Circle's VCHIQ, on the linked-in test stream
// (720p30, High, B-frames):
//
//   (default)		play it on the panel, looping: the decoder feeds the ISP
//			(scaling to 320x180 and converting to RGB565 in the
//			VideoCore), the ARM shows each frame at its time
//   videodec=rgba	the ISP makes 1024x576 RGBA (a screen-sized texture) in ARM
//			memory: first as fast as it goes (300 frames, measured),
//			then played at 30 fps with a preview on the panel (the
//			ARM downsamples it: for the test only)
//   videodec=bench	decode to I420 on the ARM side as fast as it goes, count
//			the frames, dump frame 100's luma to the log
//
// The stream is 720p (default) or 1080p (make VIDEO=1080).
//
#include "kernel.h"
#include <circle/string.h>
#include <circle/util.h>
#include <circle/synchronize.h>

extern "C" {
#include "interface/mmal/mmal.h"
#include "interface/mmal/util/mmal_util.h"
#include "interface/mmal/util/mmal_util_params.h"
#include "interface/mmal/util/mmal_connection.h"
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
	m_VCHIQ (CMemorySystem::Get (), &m_Interrupt),
	m_Panel (&m_Interrupt)
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
	       && m_VCHIQ.Initialize ()
	       && m_Panel.Initialize ();
}

TShutdownMode CKernel::Run (void)
{
	LOGNOTE ("videodec built " __DATE__ " " __TIME__ ", stream %u bytes",
		 (unsigned) (test_stream_end - test_stream));

	const char *pMode = m_Options.GetAppOptionString ("videodec", "play");
	if (strcmp (pMode, "bench") == 0)
	{
		Decode ();
	}
	else
	{
		Play (strcmp (pMode, "rgba") == 0);
	}

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
	pFormat->es->video.width = VIDEO_WIDTH;
	pFormat->es->video.height = VIDEO_HEIGHT;
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

// ---- play on the panel ------------------------------------------------------------

#define FRAME_US	33333		// 30 fps
#define PANEL_WIDTH	320
#define PANEL_HEIGHT	240
#define PREVIEW_WIDTH	320		// the picture on the panel: 16:9, letterboxed
#define PREVIEW_HEIGHT	180
#define RGBA_WIDTH	1024		// rgba: a 16:9 picture for the 1024x600 monitor
#define RGBA_HEIGHT	576
#define MEASURE_FRAMES	300
#define OUT_BUFFERS	4		// decoded frames waiting for their time

static u16 s_PanelFrame[2][PANEL_WIDTH * PANEL_HEIGHT] ALIGN (64);

void CKernel::Play (boolean bRGBA)
{
	const unsigned OUT_WIDTH = bRGBA ? RGBA_WIDTH : PREVIEW_WIDTH;
	const unsigned OUT_HEIGHT = bRGBA ? RGBA_HEIGHT : PREVIEW_HEIGHT;

	MMAL_STATUS_T s = mmal_vc_init ();
	CHECK (s, "mmal_vc_init");

	MMAL_COMPONENT_T *pDecoder, *pISP;
	s = mmal_component_create ("vc.ril.video_decode", &pDecoder);
	CHECK (s, "create vc.ril.video_decode");
	s = mmal_component_create ("vc.ril.isp", &pISP);
	CHECK (s, "create vc.ril.isp");
	mmal_port_enable (pDecoder->control, ControlCallback);
	mmal_port_enable (pISP->control, ControlCallback);

	MMAL_PORT_T *pIn = pDecoder->input[0];
	MMAL_ES_FORMAT_T *pFormat = pIn->format;
	pFormat->type = MMAL_ES_TYPE_VIDEO;
	pFormat->encoding = MMAL_ENCODING_H264;
	pFormat->es->video.width = VIDEO_WIDTH;
	pFormat->es->video.height = VIDEO_HEIGHT;
	pFormat->es->video.frame_rate.num = 30;
	pFormat->es->video.frame_rate.den = 1;
	pFormat->es->video.par.num = 1;
	pFormat->es->video.par.den = 1;
	s = mmal_port_format_commit (pIn);
	CHECK (s, "decoder input format");
	s = mmal_port_format_commit (pDecoder->output[0]);
	CHECK (s, "decoder output format");

	// decoder -> ISP inside the VideoCore
	MMAL_CONNECTION_T *pConnection;
	s = mmal_connection_create (&pConnection, pDecoder->output[0], pISP->input[0],
				    MMAL_CONNECTION_FLAG_TUNNELLING);
	CHECK (s, "connect decoder to ISP");
	s = mmal_connection_enable (pConnection);
	CHECK (s, "enable connection");

	// the ISP's output: the panel's picture in RGB565, to the ARM
	MMAL_PORT_T *pOut = pISP->output[0];
	mmal_format_copy (pOut->format, pISP->input[0]->format);
	pOut->format->encoding = bRGBA ? MMAL_ENCODING_RGBA : MMAL_ENCODING_RGB16;
	pOut->format->es->video.width = VCOS_ALIGN_UP (OUT_WIDTH, 32);
	pOut->format->es->video.height = VCOS_ALIGN_UP (OUT_HEIGHT, 16);
	pOut->format->es->video.crop.x = 0;
	pOut->format->es->video.crop.y = 0;
	pOut->format->es->video.crop.width = OUT_WIDTH;
	pOut->format->es->video.crop.height = OUT_HEIGHT;
	s = mmal_port_format_commit (pOut);
	CHECK (s, "ISP output format");

	pIn->buffer_num = pIn->buffer_num_recommended;
	pIn->buffer_size = CHUNK;
	pOut->buffer_num = OUT_BUFFERS > pOut->buffer_num_min ? OUT_BUFFERS : pOut->buffer_num_min;
	pOut->buffer_size = pOut->buffer_size_recommended;
	LOGNOTE ("ISP output %ux%u (%4.4s): %u buffers of %u", pOut->format->es->video.width,
		 pOut->format->es->video.height, (const char *) &pOut->format->encoding,
		 pOut->buffer_num, pOut->buffer_size);

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
	s = mmal_component_enable (pISP);
	CHECK (s, "enable ISP");
	s = mmal_component_enable (pDecoder);
	CHECK (s, "enable decoder");

	memset (s_PanelFrame, 0, sizeof s_PanelFrame);
	CleanAndInvalidateDataCacheRange ((uintptr) s_PanelFrame, sizeof s_PanelFrame);

	// frames in display order, frame N shown at start + N frame times
	MMAL_BUFFER_HEADER_T *Waiting[OUT_BUFFERS + 4];
	unsigned nWaiting = 0, nPanel = 0;
	u64 nFrame = 0;			// the next frame's number
	u64 nStart = 0;			// the first frame's time
	unsigned nShown = 0, nDropped = 0, nLateUs = 0, nMaxLateUs = 0;
	u64 nLastLog = CTimer::GetClockTicks64 ();
	const u8 *pData = test_stream;
	unsigned nMeasured = bRGBA ? 0 : MEASURE_FRAMES;	// rgba: first as fast as it goes
	u64 nMeasureStart = 0;
	while (1)
	{
		m_DevLink.Update ();

		// decoded frames (and format changes, reported)
		MMAL_BUFFER_HEADER_T *pBuffer;
		while ((pBuffer = mmal_queue_get (s_pOutQueue)) != nullptr)
		{
			if (!pBuffer->cmd && pBuffer->length && nMeasured < MEASURE_FRAMES)
			{
				u64 nNow = CTimer::GetClockTicks64 ();
				if (!nMeasured++)
				{
					nMeasureStart = nNow;
					LOGNOTE ("first RGBA frame: %u bytes at %p", pBuffer->length, pBuffer->data);
				}
				else if (nMeasured == MEASURE_FRAMES)
				{
					unsigned nUs = (unsigned) (nNow - nMeasureStart);
					LOGNOTE ("%u RGBA %ux%u frames from %ux%u: %u.%u fps (%u MB/s into ARM memory)",
						 MEASURE_FRAMES - 1, OUT_WIDTH, OUT_HEIGHT, VIDEO_WIDTH,
						 VIDEO_HEIGHT, (MEASURE_FRAMES - 1) * 1000000U / nUs,
						 (unsigned) ((u64) (MEASURE_FRAMES - 1) * 10000000 / nUs % 10),
						 (unsigned) ((u64) (MEASURE_FRAMES - 1) * pBuffer->length / nUs));
				}
				mmal_buffer_header_release (pBuffer);
				continue;
			}
			if (pBuffer->cmd)
			{
				LOGNOTE ("event %4.4s on the ISP output", (const char *) &pBuffer->cmd);
				mmal_buffer_header_release (pBuffer);
			}
			else if (!pBuffer->length || nWaiting == sizeof Waiting / sizeof Waiting[0])
			{
				mmal_buffer_header_release (pBuffer);
			}
			else
			{
				Waiting[nWaiting++] = pBuffer;
			}
		}

		// the frame that's due: late ones are dropped for the newest due one
		u64 nNow = CTimer::GetClockTicks64 ();
		if (nMeasured < MEASURE_FRAMES)
		{
			nWaiting = 0;
		}
		else if (nWaiting && !nStart)
		{
			nStart = nNow;
		}
		unsigned nDue = 0;
		while (nDue < nWaiting && nStart + (nFrame + nDue) * FRAME_US <= nNow)
		{
			nDue++;
		}
		if (nDue)
		{
			for (unsigned i = 0; i < nDue - 1; i++)
			{
				mmal_buffer_header_release (Waiting[i]);
				nDropped++;
			}
			pBuffer = Waiting[nDue - 1];
			nFrame += nDue;
			unsigned nLate = (unsigned) (nNow - (nStart + (nFrame - 1) * FRAME_US));
			nLateUs += nLate;
			nMaxLateUs = nLate > nMaxLateUs ? nLate : nMaxLateUs;

			// letterboxed into the panel's frame, then out by DMA
			m_Panel.WaitIdle ();
			u16 *pPanel = s_PanelFrame[nPanel];
			unsigned nStride = pOut->format->es->video.width;
			mmal_buffer_header_mem_lock (pBuffer);
			unsigned nTop = (PANEL_HEIGHT - PREVIEW_HEIGHT) / 2;
			if (!bRGBA)
			{
				const u16 *pRGB = (const u16 *) (pBuffer->data + pBuffer->offset);
				for (unsigned y = 0; y < OUT_HEIGHT; y++)
				{
					memcpy (pPanel + (nTop + y) * PANEL_WIDTH, pRGB + y * nStride,
						OUT_WIDTH * 2);
				}
			}
			else		// the preview: every 3.2th pixel, to RGB565
			{
				const u32 *pRGBA = (const u32 *) (pBuffer->data + pBuffer->offset);
				CleanAndInvalidateDataCacheRange ((uintptr) pRGBA, pBuffer->length);
				for (unsigned y = 0; y < PREVIEW_HEIGHT; y++)
				{
					const u32 *pRow = pRGBA + (y * OUT_HEIGHT / PREVIEW_HEIGHT) * nStride;
					u16 *pDest = pPanel + (nTop + y) * PANEL_WIDTH;
					for (unsigned x = 0; x < PREVIEW_WIDTH; x++)
					{
						u32 c = pRow[x * OUT_WIDTH / PREVIEW_WIDTH];	// R in byte 0
						pDest[x] = (c << 8 & 0xF800) | (c >> 5 & 0x07E0) | (c >> 19 & 0x001F);
					}
				}
			}
			mmal_buffer_header_mem_unlock (pBuffer);
			mmal_buffer_header_release (pBuffer);
			CleanAndInvalidateDataCacheRange ((uintptr) pPanel, sizeof s_PanelFrame[0]);
			m_Panel.Show (pPanel, nullptr, nullptr);
			nPanel ^= 1;
			nShown++;

			nWaiting -= nDue;
			memmove (Waiting, Waiting + nDue, nWaiting * sizeof Waiting[0]);
		}

		// buffers for the ISP to fill
		while ((pBuffer = mmal_queue_get (pPoolOut->queue)) != nullptr)
		{
			mmal_port_send_buffer (pOut, pBuffer);
		}

		// the stream, round and round (one continuous stream to the decoder)
		while ((pBuffer = mmal_queue_get (pPoolIn->queue)) != nullptr)
		{
			unsigned nLength = test_stream_end - pData;
			if (nLength > pBuffer->alloc_size)
			{
				nLength = pBuffer->alloc_size;
			}
			memcpy (pBuffer->data, pData, nLength);
			pData += nLength;
			if (pData == test_stream_end)
			{
				pData = test_stream;
			}
			pBuffer->length = nLength;
			pBuffer->offset = 0;
			pBuffer->pts = pBuffer->dts = MMAL_TIME_UNKNOWN;
			pBuffer->flags = 0;
			mmal_port_send_buffer (pIn, pBuffer);
		}

		if (nNow - nLastLog >= 1000000)
		{
			LOGNOTE ("shown %u, dropped %u, late %u us on average (max %u), %u waiting",
				 nShown, nDropped, nShown ? nLateUs / nShown : 0, nMaxLateUs, nWaiting);
			nShown = nDropped = nLateUs = nMaxLateUs = 0;
			nLastLog = nNow;
		}

		m_Scheduler.Yield ();
	}
}

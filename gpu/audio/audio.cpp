//
// audio.cpp
//
#include "audio.h"
#include "aac.h"
#include <pgpu_protocol.h>
#include <circle/logger.h>
#include <circle/timer.h>
#include <circle/synchronize.h>
#include <circle/util.h>
#include <assert.h>

LOGMODULE ("audio");

#define STATUS_US	100000		// VIDEO_STATUS while open (as a video stream's)
#define DECODE_US	2000		// the main loop's turn when no core of its own decodes
#define PARK_US		500000		// the most Close waits for the decoder to let go

// the frames the VideoCore holds after GetChunk has handed them over: Circle's
// VCHIQ sound queues a chunk more when one is left, so two chunks; the one
// playing is then moving, so the time since that GetChunk (up to a chunk) is
// added: the clock runs smoothly between chunks, not in 43 ms steps

CAudio::CAudio (CVCHIQDevice *pVCHIQ, CVideo *pVideo)
:	m_pVCHIQ (pVCHIQ),
	m_pVideo (pVideo),
	m_nDefaultVolume (10),
	m_bOwnCore (FALSE),
	m_bOpen (FALSE),
	m_nVideoStream (0),
	m_pAAC (nullptr),
	m_pOut (nullptr),
	m_pLastOut (nullptr),
	m_nRate (0),
	m_bPaused (FALSE),
	m_nVolume (10),
	m_nSilenceAtOpen (0),
	m_pRing (new u8[RingBytes]),
	m_nBytesIn (0),
	m_nSamplesIn (0),
	m_bPartial (FALSE),
	m_nPartialBytes (0),
	m_bSkipping (FALSE),
	m_nBytesDone (0),
	m_nSamplesDone (0),
	m_nMarks (0),
	m_bPark (TRUE),
	m_bParked (TRUE),
	m_nDecoded (0),
	m_nErrors (0),
	m_bEOS (FALSE),
	m_pUnit (new u8[MaxUnit]),
	m_pPCM (new s16[2 * AAC_MAX_FRAMES]),
	m_nLastStatus (0),
	m_nLastDebug (0)
{
	assert (m_pRing && m_pUnit && m_pPCM);
	memset (m_pOuts, 0, sizeof m_pOuts);
}

CAudio::~CAudio (void)
{
	Close ();
	delete [] m_pPCM;
	delete [] m_pUnit;
	delete [] m_pRing;
}

void CAudio::SetDefaultVolume (unsigned nPercent)
{
	m_nDefaultVolume = nPercent > 100 ? 100 : nPercent;
}

u32 CAudio::Open (u32 nCodec, unsigned nVideoStream, const u8 *pConfig, unsigned nConfigBytes)
{
	Close ();
	if (nCodec != PGPU_AUDIO_AAC)
	{
		return PGPU_ERR_ENUM;
	}
	if (nVideoStream > CVideo::MaxStreams)
	{
		return PGPU_ERR_ID;
	}

	unsigned nRate, nChannels;
	aac_t *pAAC = aac_open (pConfig, nConfigBytes, &nRate, &nChannels);
	if (!pAAC)
	{
		LOGWARN ("The AAC decoder doesn't take this configuration (%u bytes)", nConfigBytes);
		return PGPU_ERR_ENUM;
	}

	CAudioOut *pOut = GetOutput (nRate);
	if (!pOut)
	{
		aac_close (pAAC);
		return PGPU_ERR_LIMIT;
	}
	pOut->Flush ();
	pOut->SetPaused (FALSE);
	if (!pOut->IsActive () && !pOut->Start ())
	{
		LOGWARN ("No sound output at %u Hz", nRate);
		aac_close (pAAC);
		return PGPU_ERR_OBJECT;
	}

	m_pAAC = pAAC;
	m_pOut = pOut;
	m_nRate = nRate;
	m_bPaused = FALSE;
	m_nVideoStream = nVideoStream;
	m_nVolume = m_nDefaultVolume;
	aac_set_volume (m_pAAC, m_nVolume * 65536 / 100);
	m_nSilenceAtOpen = pOut->GetUnderrunFrames ();
	m_nBytesIn = 0;
	m_nSamplesIn = 0;
	m_bPartial = FALSE;
	m_bSkipping = FALSE;
	m_nBytesDone = 0;
	m_nSamplesDone = 0;
	m_nMarks = 0;
	m_nDecoded = 0;
	m_nErrors = 0;
	m_bEOS = FALSE;

	// the decoder may take it now
	DataMemBarrier ();
	m_bOpen = TRUE;
	m_bPark = FALSE;

	if (m_nVideoStream)
	{
		m_pVideo->SetClock (m_nVideoStream, this);
	}
	LOGNOTE ("Stream: AAC, %u Hz, %u channels (as stereo), volume %u%%, %s%s", nRate, nChannels,
		 m_nVolume, m_bOwnCore ? "decoded on a core of its own" : "decoded in the main loop",
		 m_nVideoStream ? ", the video's clock" : "");

	return 0;
}

// the output for this rate (the VideoCore's audio service is set up once a
// device; a stopped one starts again)
CAudioOut *CAudio::GetOutput (unsigned nRate)
{
	for (unsigned i = 0; i < MaxRates; i++)
	{
		if (!m_pOuts[i])
		{
			m_pOuts[i] = new CAudioOut (m_pVCHIQ, nRate);
		}
		if (m_pOuts[i] && m_pOuts[i]->GetSampleRate () == nRate)
		{
			m_pLastOut = m_pOuts[i];
			return m_pOuts[i];
		}
	}
	return nullptr;
}

void CAudio::Close (void)
{
	if (!m_bOpen)
	{
		return;
	}
	if (m_nVideoStream)
	{
		m_pVideo->SetClock (m_nVideoStream, nullptr);
	}

	// the decoder lets go of the stream (store, then load: the decoder does
	// the same the other way round, so one of them sees the other)
	m_bPark = TRUE;
	DataMemBarrier ();
	if (m_bOwnCore)
	{
		unsigned nStart = CTimer::GetClockTicks ();
		while (!m_bParked && CTimer::GetClockTicks () - nStart < PARK_US)
		{
			DataMemBarrier ();
		}
		if (!m_bParked)
		{
			LOGWARN ("The decoder didn't let go");
		}
	}

	m_bOpen = FALSE;
	m_pOut->Flush ();
	m_pOut->Cancel ();			// (waits for the VideoCore: silence from here)
	aac_close (m_pAAC);
	m_pAAC = nullptr;
	m_pOut = nullptr;
	LOGNOTE ("Stream closed: %u units decoded, %u broken", m_nDecoded, m_nErrors);
}

u32 CAudio::Data (u32 nFlags, s64 nPTS, unsigned nSampleBytes, const u8 *pData, unsigned nBytes)
{
	if (!m_bOpen)
	{
		return PGPU_ERR_OBJECT;
	}

	if (nFlags & PGPU_VIDEO_FIRST)
	{
		if (m_bPartial)				// the last one lost its end: dropped
		{
			m_nBytesIn -= m_nPartialBytes;
			m_bPartial = FALSE;
		}
		m_bSkipping = FALSE;
		u32 nSamplesDone = m_nSamplesDone, nBytesDone = m_nBytesDone;
		if (   m_nSamplesIn - nSamplesDone >= MaxSamples || nSampleBytes < nBytes
		    || nSampleBytes > MaxUnit || m_nBytesIn - nBytesDone + nSampleBytes > RingBytes)
		{
			m_bSkipping = !(nFlags & PGPU_VIDEO_LAST);
			return PGPU_ERR_LIMIT;		// more than the host was told it could send
		}
		TSample &S = m_Samples[m_nSamplesIn % MaxSamples];
		S.nOffset = m_nBytesIn;
		S.nSize = nSampleBytes;
		S.nPTS = nPTS;
		S.nFlags = nFlags;
		m_bPartial = TRUE;
		m_nPartialBytes = 0;
	}
	else
	{
		if (m_bSkipping)
		{
			m_bSkipping = !(nFlags & PGPU_VIDEO_LAST);
			return 0;
		}
		if (!m_bPartial)
		{
			return PGPU_ERR_LIMIT;		// no sample started
		}
	}

	TSample &S = m_Samples[m_nSamplesIn % MaxSamples];
	if (m_nPartialBytes + nBytes > S.nSize)
	{
		return PGPU_ERR_LIMIT;			// more than it said (the rest is dropped)
	}
	for (unsigned i = 0; i < nBytes; )
	{
		unsigned nAt = m_nBytesIn % RingBytes;
		unsigned n = RingBytes - nAt < nBytes - i ? RingBytes - nAt : nBytes - i;
		memcpy (m_pRing + nAt, pData + i, n);
		m_nBytesIn += n;
		i += n;
	}
	m_nPartialBytes += nBytes;

	if (nFlags & PGPU_VIDEO_LAST)
	{
		S.nSize = m_nPartialBytes;		// (fewer bytes than it said: those)
		S.nFlags |= nFlags & PGPU_VIDEO_EOS;
		m_bPartial = FALSE;
		DataMemBarrier ();			// the sample before the count that shows it
		m_nSamplesIn = m_nSamplesIn + 1;
	}

	return 0;
}

u32 CAudio::Control (u32 nOp, s64 nArg)
{
	if (!m_bOpen)
	{
		return nOp == PGPU_VIDEO_CLOSE ? 0 : PGPU_ERR_OBJECT;
	}

	switch (nOp)
	{
	case PGPU_VIDEO_PLAY:			// (the time: the samples' own)
		m_bPaused = FALSE;
		m_pOut->SetPaused (FALSE);
		return 0;

	case PGPU_VIDEO_PAUSE:			// the sound, and so the video's clock, stops
		m_bPaused = TRUE;
		m_pOut->SetPaused (TRUE);
		return 0;

	case PGPU_VIDEO_CLOSE:
		Close ();
		return 0;

	case PGPU_AUDIO_VOLUME:
		if (nArg < 0 || nArg > 100)
		{
			return PGPU_ERR_LIMIT;
		}
		m_nVolume = (unsigned) nArg;
		aac_set_volume (m_pAAC, m_nVolume * 65536 / 100);	// (from the next unit)
		return 0;

	default:
		return PGPU_ERR_ENUM;
	}
}

void CAudio::CopyFromRing (u32 nOffset, u8 *pTo, unsigned nBytes) const
{
	for (unsigned i = 0; i < nBytes; )
	{
		unsigned nAt = (nOffset + i) % RingBytes;
		unsigned n = RingBytes - nAt < nBytes - i ? RingBytes - nAt : nBytes - i;
		memcpy (pTo + i, m_pRing + nAt, n);
		i += n;
	}
}

// one complete sample, if there is one and its frames fit
boolean CAudio::DecodeOne (void)
{
	u32 nIn = m_nSamplesIn;
	DataMemBarrier ();			// the count before the samples it shows
	u32 nDone = m_nSamplesDone;
	if (nDone == nIn || m_pOut->Room () < AAC_MAX_FRAMES)
	{
		return FALSE;
	}

	const TSample &S = m_Samples[nDone % MaxSamples];
	if (S.nSize)
	{
		CopyFromRing (S.nOffset, m_pUnit, S.nSize);
		int nFrames = aac_decode (m_pAAC, m_pUnit, S.nSize, m_pPCM);
		if (nFrames < 0)
		{
			if (m_nErrors++ < 10)
			{
				LOGWARN ("A broken unit at %d ms: %s", (int) (S.nPTS / 1000), aac_error (m_pAAC));
			}
		}
		else if (nFrames > 0)
		{
			// its time at its first frame, then the frames
			TMark &M = m_Marks[m_nMarks % MaxMarks];
			M.nFrame = m_pOut->GetWritten ();
			M.nPTS = S.nPTS;
			DataMemBarrier ();
			m_nMarks = m_nMarks + 1;
			m_pOut->Write (m_pPCM, nFrames);
		}
		m_nDecoded = m_nDecoded + 1;
	}
	if (S.nFlags & PGPU_VIDEO_EOS)
	{
		m_bEOS = TRUE;
	}

	// the sample's room back to the host
	m_nBytesDone = m_nBytesDone + S.nSize;
	DataMemBarrier ();
	m_nSamplesDone = nDone + 1;

	return TRUE;
}

void CAudio::DecodeLoop (void)
{
	m_bOwnCore = TRUE;
	while (1)
	{
		if (m_bPark || !m_bOpen)
		{
			m_bParked = TRUE;
			DataMemBarrier ();
			CTimer::SimpleusDelay (200);
			continue;
		}

		// store, then load (Close does the same the other way round)
		m_bParked = FALSE;
		DataMemBarrier ();
		if (m_bPark)
		{
			continue;
		}

		if (!DecodeOne ())
		{
			CTimer::SimpleusDelay (500);	// (a unit is 21 ms of sound)
		}
	}
}

void CAudio::Update (void)
{
	if (!m_bOpen)
	{
		return;
	}

	if (!m_bOwnCore)
	{
		unsigned nStart = CTimer::GetClockTicks ();
		while (CTimer::GetClockTicks () - nStart < DECODE_US && DecodeOne ())
		{
		}
	}

	u64 nNow = CTimer::GetClockTicks64 ();
	if (nNow - m_nLastDebug >= 1000000)
	{
		m_nLastDebug = nNow;
		s64 nTime;
		boolean bTime = GetTime (&nTime);
		LOGDBG ("%u units decoded, %u broken, %u ms queued, %u ms of silence, heard %d ms",
			m_nDecoded, m_nErrors, m_pOut->Queued () * 1000 / m_nRate,
			(unsigned) ((m_pOut->GetUnderrunFrames () - m_nSilenceAtOpen) * 1000 / m_nRate),
			bTime ? (int) (nTime / 1000) : -1);
	}
}

boolean CAudio::GetTime (s64 *pUS)
{
	if (!m_bOpen)
	{
		return FALSE;
	}
	u32 nMarks = m_nMarks;
	DataMemBarrier ();			// the count before the marks it shows
	if (!nMarks)
	{
		return FALSE;
	}

	// the frame being heard, and the newest mark at or before it (the oldest
	// few may be rewritten meanwhile: not those)
	unsigned nChunk = m_pOut->GetChunkFrames ();
	u32 nRead;
	unsigned nTime;
	do					// (a consistent pair: GetChunk may come between)
	{
		nTime = m_pOut->GetChunkTime ();
		nRead = m_pOut->GetRead ();
	}
	while (nTime != m_pOut->GetChunkTime ());
	u64 nSince = (u64) (CTimer::GetClockTicks () - nTime) * m_nRate / 1000000;
	u32 nHeard = nRead - 2 * nChunk + (nSince < nChunk ? (u32) nSince : nChunk);
	u32 nOldest = nMarks > MaxMarks - 8 ? nMarks - (MaxMarks - 8) : 0;
	for (u32 k = nMarks; k-- > nOldest; )
	{
		const TMark &M = m_Marks[k % MaxMarks];
		s32 nSince = (s32) (nHeard - M.nFrame);
		if (nSince >= 0)
		{
			*pUS = M.nPTS + (s64) nSince * 1000000 / m_nRate;
			return TRUE;
		}
	}

	return FALSE;				// its first frame isn't heard yet
}

boolean CAudio::GetStatus (u32 *pPayload, boolean bDue)
{
	u64 nNow = CTimer::GetClockTicks64 ();
	if (bDue && (!m_bOpen || nNow - m_nLastStatus < STATUS_US))
	{
		return FALSE;
	}
	m_nLastStatus = nNow;

	s64 nHeard;
	if (!GetTime (&nHeard))
	{
		nHeard = PGPU_VIDEO_TIME_NONE;
	}
	u32 nSamplesDone = m_nSamplesDone;
	pPayload[0] = PGPU_AUDIO_STREAM;
	pPayload[1] =   (m_bOpen ? PGPU_VIDEO_OPEN_FLAG : 0)
		      | (m_bOpen && !m_bPaused ? PGPU_VIDEO_PLAYING : 0)
		      | (m_bOpen && m_bEOS && nSamplesDone == m_nSamplesIn ? PGPU_VIDEO_ENDED : 0);
	pPayload[2] = m_nBytesDone;
	pPayload[3] = RingBytes;
	pPayload[4] = m_nDecoded;
	pPayload[5] = m_pOut ? (u32) ((m_pOut->GetUnderrunFrames () - m_nSilenceAtOpen) * 1000 / m_nRate) : 0;
	pPayload[6] = m_nErrors;
	pPayload[7] = (u32) nHeard;
	pPayload[8] = (u32) ((u64) nHeard >> 32);
	pPayload[9] = m_pOut ? m_pOut->Queued () * 1000 / m_nRate : 0;
	pPayload[10] = nSamplesDone;
	pPayload[11] = MaxSamples;

	return TRUE;
}

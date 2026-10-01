//
// audio.cpp
//
#include "audio.h"
#include "aac.h"
#include "mp3.h"
#include "vorbis.h"
#include <pgpu_protocol.h>
#include <circle/logger.h>
#include <circle/timer.h>
#include <circle/synchronize.h>
#include <circle/util.h>
#include <assert.h>
#include <math.h>

LOGMODULE ("audio");

#define MAX_FRAMES	(AAC_MAX_FRAMES > MP3_MAX_FRAMES ? AAC_MAX_FRAMES : MP3_MAX_FRAMES)
#define PCM_FRAMES	(MAX_FRAMES > VORBIS_CHUNK_FRAMES ? MAX_FRAMES : VORBIS_CHUNK_FRAMES)

#define STATUS_US	100000		// MEDIA_STATUS while open (as a video stream's)
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
	m_bMute (FALSE),
	m_bOwnCore (FALSE),
	m_bOpen (FALSE),
	m_nVideoStream (0),
	m_nCodec (0),
	m_pAAC (nullptr),
	m_pMP3 (nullptr),
	m_pVorbis (nullptr),
	m_nMaxFrames (AAC_MAX_FRAMES),
	m_pOut (nullptr),
	m_pLastOut (nullptr),
	m_pLastSink (nullptr),
	m_pOther (nullptr),
	m_pSoundSink (nullptr),
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
	m_pPCM (new s16[2 * PCM_FRAMES]),
	m_bPageFed (FALSE),
	m_nPageFrames (0),
	m_nLosses (0),
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

void CAudio::SetVolume (unsigned nPercent)
{
	SetDefaultVolume (nPercent);
	m_nVolume = m_nDefaultVolume;
	SetOutputVolume ();
}

// ---- the sound effects -----------------------------------------------------------------
//
// They're mixed into one output: the stream's while there is one; else one
// kept running for them (the speaker's while it's there, else HDMI's at
// 48 kHz), from the first sound the host sends till its session ends.

u32 CAudio::SoundData (unsigned nId, unsigned nRate, unsigned nFrames, unsigned nOffset, u32 nFormat,
		       const u8 *pData, unsigned nBytes)
{
	u32 nError = m_Sounds.Data (nId, nRate, nFrames, nOffset, nFormat, pData, nBytes);
	UpdateSounds ();
	return nError;
}

u32 CAudio::SoundDelete (unsigned nId)
{
	u32 nError = m_Sounds.Delete (nId);
	UpdateSounds ();
	return nError;
}

u32 CAudio::SoundPlay (unsigned nChannel, unsigned nId, unsigned nLeft, unsigned nRight, u32 nFlags)
{
	return m_Sounds.Play (nChannel, nId, nLeft, nRight, nFlags);
}

void CAudio::SoundReset (void)
{
	m_Sounds.Reset ();
	UpdateSounds ();
}

void CAudio::UpdateSounds (void)
{
	if (m_pOut)				// the stream's output plays them
	{
		CAudioSink::SetSounds (&m_Sounds, m_pOut);
		return;
	}
	if (!m_Sounds.IsLoaded ())
	{
		if (m_pSoundSink)		// (no more of them: its output off)
		{
			CAudioSink::SetSounds (&m_Sounds, nullptr);
			m_pSoundSink->Close ();
			m_pSoundSink = nullptr;
		}
		return;
	}
	CAudioSink *pSink = GetOutput (48000);
	if (pSink && pSink != m_pSoundSink)	// (the first sound; or the speaker came, or went)
	{
		if (m_pSoundSink)
		{
			m_pSoundSink->Close ();
		}
		pSink->Flush ();
		pSink->SetPaused (FALSE);
		if (pSink->Open ())
		{
			m_pSoundSink = pSink;
			m_nVolume = m_nDefaultVolume;
			SetOutputVolume ();
		}
		else
		{
			m_pSoundSink = nullptr;
		}
	}
	CAudioSink::SetSounds (&m_Sounds, m_pSoundSink);
}

void CAudio::SetMute (boolean bMute)
{
	m_bMute = bMute;
	SetOutputVolume ();
}

boolean CAudio::PlayTest (void)
{
	static const unsigned Rate = 48000, NoteFrames = Rate * 3 / 10, GapFrames = Rate / 10, FadeFrames = Rate / 100;
	static const float Hz[3] = {523.25f, 659.25f, 783.99f};		// C5, E5, G5
	if (m_bOpen)
	{
		return FALSE;
	}
	CAudioSink *pOut = GetOutput (Rate);
	if (!pOut || !pOut->Open ())
	{
		return FALSE;
	}
	pOut->Flush ();
	pOut->SetPaused (FALSE);
	m_nVolume = m_nDefaultVolume;
	SetOutputVolume ();

	unsigned nFrames = 3 * (NoteFrames + GapFrames);
	s16 *pFrames = new s16[2 * nFrames];
	memset (pFrames, 0, 2 * nFrames * sizeof (s16));
	for (unsigned nNote = 0; nNote < 3; nNote++)			// left, right, both
	{
		s16 *p = pFrames + 2 * nNote * (NoteFrames + GapFrames);
		for (unsigned i = 0; i < NoteFrames; i++)
		{
			unsigned nEdge = i < NoteFrames - 1 - i ? i : NoteFrames - 1 - i;
			float fGain = nEdge < FadeFrames ? (float) nEdge / FadeFrames : 1.0f;	// (no clicks)
			s16 nValue = (s16) (16000.0f * fGain * sinf (6.2831853f * Hz[nNote] * i / Rate));
			p[2 * i] = nNote != 1 ? nValue : 0;
			p[2 * i + 1] = nNote != 0 ? nValue : 0;
		}
	}
	pOut->Write (pFrames, nFrames);
	delete [] pFrames;
	return TRUE;
}

u32 CAudio::Open (u32 nCodec, unsigned nVideoStream, const u8 *pConfig, unsigned nConfigBytes)
{
	Close ();
	if (nCodec != PGPU_AUDIO_AAC && nCodec != PGPU_AUDIO_MP3 && nCodec != PGPU_AUDIO_VORBIS)
	{
		return PGPU_ERR_ENUM;
	}
	if (nVideoStream > CVideo::MaxStreams)
	{
		return PGPU_ERR_ID;
	}

	unsigned nRate, nChannels;
	m_nCodec = nCodec;
	if (nCodec == PGPU_AUDIO_AAC)
	{
		m_pAAC = aac_open (pConfig, nConfigBytes, &nRate, &nChannels);
		m_nMaxFrames = AAC_MAX_FRAMES;
	}
	else if (nCodec == PGPU_AUDIO_MP3)
	{
		m_pMP3 = mp3_open (pConfig, nConfigBytes, &nRate, &nChannels);
		m_nMaxFrames = MP3_MAX_FRAMES;
	}
	else
	{
		m_pVorbis = vorbis_open (pConfig, nConfigBytes, &nRate, &nChannels);
		m_nMaxFrames = VORBIS_CHUNK_FRAMES;
	}
	if (!m_pAAC && !m_pMP3 && !m_pVorbis)
	{
		LOGWARN ("The %s decoder doesn't take this configuration (%u bytes)", CodecName (nCodec), nConfigBytes);
		return PGPU_ERR_ENUM;
	}

	CAudioSink *pOut = GetOutput (nRate);
	if (!pOut)
	{
		CloseDecoder ();
		return PGPU_ERR_LIMIT;
	}
	pOut->Flush ();
	pOut->SetPaused (FALSE);
	if (!pOut->Open ())
	{
		LOGWARN ("No sound output at %u Hz", nRate);
		CloseDecoder ();
		return PGPU_ERR_OBJECT;
	}

	m_pOut = pOut;
	if (m_pSoundSink && m_pSoundSink != pOut)	// (the effects go with the stream now)
	{
		m_pSoundSink->Close ();
	}
	m_pSoundSink = nullptr;
	CAudioSink::SetSounds (&m_Sounds, pOut);
	m_nRate = nRate;
	m_bPaused = FALSE;
	m_nVideoStream = nVideoStream;
	m_nVolume = m_nDefaultVolume;
	SetOutputVolume ();
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
	m_bPageFed = FALSE;
	m_nLosses = 0;

	// the decoder may take it now
	DataMemBarrier ();
	m_bOpen = TRUE;
	m_bPark = FALSE;

	if (m_nVideoStream)
	{
		m_pVideo->SetClock (m_nVideoStream, this);
	}
	LOGNOTE ("Stream: %s, %u Hz, %u channels (as stereo), volume %u%%, %s%s",
		 CodecName (nCodec), nRate, nChannels,
		 m_nVolume, m_bOwnCore ? "decoded on a core of its own" : "decoded in the main loop",
		 m_nVideoStream ? ", the video's clock" : "");

	return 0;
}

// the output for this rate (the VideoCore's audio service is set up once a
// device; a stopped one starts again)
CAudioSink *CAudio::GetOutput (unsigned nRate)
{
	if (m_pOther && m_pOther->SetSource (nRate))
	{
		m_pLastSink = m_pOther;
		return m_pOther;
	}
	m_pLastSink = GetHDMIOutput (nRate);
	return m_pLastSink;
}

CAudioOut *CAudio::GetHDMIOutput (unsigned nRate)
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
	if (!m_Sounds.IsLoaded ())		// (with sound effects it goes on playing them)
	{
		m_pOut->Close ();		// (HDMI's waits for the VideoCore: silence from here)
	}
	else
	{
		m_pSoundSink = m_pOut;
	}
	CloseDecoder ();
	m_pOut = nullptr;
	LOGNOTE ("Stream closed: %u units decoded, %u broken", m_nDecoded, m_nErrors);
}

u32 CAudio::Data (u32 nFlags, s64 nPTS, unsigned nSampleBytes, const u8 *pData, unsigned nBytes)
{
	if (!m_bOpen)
	{
		return PGPU_ERR_OBJECT;
	}

	if (nFlags & PGPU_MEDIA_FIRST)
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
			m_bSkipping = !(nFlags & PGPU_MEDIA_LAST);
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
			m_bSkipping = !(nFlags & PGPU_MEDIA_LAST);
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

	if (nFlags & PGPU_MEDIA_LAST)
	{
		S.nSize = m_nPartialBytes;		// (fewer bytes than it said: those)
		S.nFlags |= nFlags & PGPU_MEDIA_EOS;
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
		return nOp == PGPU_MEDIA_CLOSE ? 0 : PGPU_ERR_OBJECT;
	}

	switch (nOp)
	{
	case PGPU_MEDIA_PLAY:			// (the time: the samples' own)
		m_bPaused = FALSE;
		m_pOut->SetPaused (FALSE);
		return 0;

	case PGPU_MEDIA_PAUSE:			// the sound, and so the video's clock, stops
		m_bPaused = TRUE;
		m_pOut->SetPaused (TRUE);
		return 0;

	case PGPU_MEDIA_CLOSE:
		Close ();
		return 0;

	case PGPU_AUDIO_VOLUME:
		if (nArg < 0 || nArg > 100)
		{
			return PGPU_ERR_LIMIT;
		}
		m_nVolume = (unsigned) nArg;
		SetOutputVolume ();			// (from the next chunk the VideoCore takes)
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
	if (nDone == nIn || m_pOut->Room () < m_nMaxFrames)
	{
		return FALSE;
	}

	const TSample &S = m_Samples[nDone % MaxSamples];
	if (S.nSize && m_nCodec == PGPU_AUDIO_VORBIS)
	{
		if (!DecodePage (S))
		{
			return TRUE;			// (the rest of it next time)
		}
	}
	else if (S.nSize)
	{
		CopyFromRing (S.nOffset, m_pUnit, S.nSize);
		int nFrames = Decode (m_pUnit, S.nSize, m_pPCM);
		if (nFrames < 0)
		{
			if (m_nErrors++ < 10)
			{
				LOGWARN ("A broken unit at %d ms: %s", (int) (S.nPTS / 1000), DecoderError ());
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
	if (S.nFlags & PGPU_MEDIA_EOS)
	{
		m_bEOS = TRUE;
	}

	// the sample's room back to the host
	m_nBytesDone = m_nBytesDone + S.nSize;
	DataMemBarrier ();
	m_nSamplesDone = nDone + 1;

	return TRUE;
}

// Vorbis: an Ogg page, of any number of packets (seconds of sound, maybe):
// fed to the decoder once, then decoded a step at a time as the output has
// room; each step's time is the page's plus the frames before it. The page
// is done (its room back to the host) when all of it is decoded
boolean CAudio::DecodePage (const TSample &S)
{
	if (!m_bPageFed)
	{
		CopyFromRing (S.nOffset, m_pUnit, S.nSize);
		vorbis_feed (m_pVorbis, m_pUnit, S.nSize);
		m_bPageFed = TRUE;
		m_nPageFrames = 0;
	}

	bool bMore;
	int nFrames = vorbis_decode (m_pVorbis, m_pPCM, m_nMaxFrames, &bMore);
	if (nFrames > 0)
	{
		TMark &M = m_Marks[m_nMarks % MaxMarks];
		M.nFrame = m_pOut->GetWritten ();
		M.nPTS = S.nPTS + (s64) m_nPageFrames * 1000000 / m_nRate;
		DataMemBarrier ();
		m_nMarks = m_nMarks + 1;
		m_pOut->Write (m_pPCM, nFrames);
		m_nPageFrames += nFrames;
	}
	if (!bMore)
	{
		return FALSE;
	}

	m_bPageFed = FALSE;
	unsigned nLosses = vorbis_losses (m_pVorbis);
	if (nLosses != m_nLosses)
	{
		if (m_nErrors++ < 10)
		{
			LOGWARN ("%u packets lost before %d ms", nLosses - m_nLosses, (int) (S.nPTS / 1000));
		}
		m_nLosses = nLosses;
	}
	m_nDecoded = m_nDecoded + 1;
	return TRUE;
}

int CAudio::Decode (const u8 *pUnit, unsigned nBytes, s16 *pFrames)
{
	return   m_nCodec == PGPU_AUDIO_AAC ? aac_decode (m_pAAC, pUnit, nBytes, pFrames)
	       : mp3_decode (m_pMP3, pUnit, nBytes, pFrames);
}

// the volume where the frames go to the VideoCore (CAudioOut), not in the
// decoders: what's decoded waits in the output's ring (a second or more),
// so a change would be heard that much later
void CAudio::SetOutputVolume (void)
{
	// (no stream: the sound effects' output, or the test sound's)
	CAudioSink *pOut = m_pOut ? m_pOut : m_pSoundSink ? m_pSoundSink : m_pLastSink;
	if (pOut)
	{
		pOut->SetVolume (m_bMute ? 0 : m_nVolume * 65536 / 100);
	}
}

const char *CAudio::DecoderError (void)
{
	return m_nCodec == PGPU_AUDIO_AAC ? aac_error (m_pAAC) : mp3_error (m_pMP3);
}

void CAudio::CloseDecoder (void)
{
	aac_close (m_pAAC);
	mp3_close (m_pMP3);
	vorbis_close (m_pVorbis);
	m_pAAC = nullptr;
	m_pMP3 = nullptr;
	m_pVorbis = nullptr;
}

const char *CAudio::CodecName (u32 nCodec)
{
	return   nCodec == PGPU_AUDIO_AAC ? "AAC"
	       : nCodec == PGPU_AUDIO_MP3 ? "MP3" : "Vorbis";
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
	u32 nHeard = nRead - m_pOut->GetLatencyFrames () + (nSince < nChunk ? (u32) nSince : nChunk);
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
		nHeard = PGPU_MEDIA_TIME_NONE;
	}
	u32 nSamplesDone = m_nSamplesDone;
	pPayload[0] = PGPU_AUDIO_STREAM;
	pPayload[1] =   (m_bOpen ? PGPU_MEDIA_OPEN_FLAG : 0)
		      | (m_bOpen && !m_bPaused ? PGPU_MEDIA_PLAYING : 0)
		      | (m_bOpen && m_bEOS && nSamplesDone == m_nSamplesIn ? PGPU_MEDIA_ENDED : 0)
		      | m_nVolume << 8;			// (the audio stream's: its volume)
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

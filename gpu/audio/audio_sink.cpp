//
// audio_sink.cpp
//
#include "audio_sink.h"
#include "sounds.h"
#include <circle/synchronize.h>
#include <circle/util.h>
#include <circle/timer.h>
#include <assert.h>

CSounds *CAudioSink::s_pSounds = nullptr;
CAudioSink *volatile CAudioSink::s_pSoundsSink = nullptr;
s16 *CAudioSink::s_pTap = nullptr;
volatile boolean CAudioSink::s_bTap = FALSE;
volatile unsigned CAudioSink::s_nTapIn = 0;
volatile unsigned CAudioSink::s_nTapOut = 0;
volatile unsigned CAudioSink::s_nTapRate = 0;
volatile unsigned CAudioSink::s_nTapLost = 0;
volatile u64 CAudioSink::s_nTapStart = 0;

void CAudioSink::StartTap (void)
{
	s_bTap = FALSE;
	if (!s_pTap)
	{
		s_pTap = new s16[TapFrames * 2];
	}
	s_nTapIn = s_nTapOut = s_nTapRate = s_nTapLost = 0;
	DataMemBarrier ();
	s_bTap = s_pTap != nullptr;
}

void CAudioSink::StopTap (void)
{
	s_bTap = FALSE;
}

unsigned CAudioSink::ReadTap (s16 *pFrames, unsigned nMaxFrames)
{
	unsigned nOut = s_nTapOut, n = s_nTapIn - nOut;
	DataMemBarrier ();
	n = n < nMaxFrames ? n : nMaxFrames;
	for (unsigned i = 0; i < n; i++, nOut++)
	{
		unsigned j = (nOut % TapFrames) * 2;
		pFrames[2 * i] = s_pTap[j];
		pFrames[2 * i + 1] = s_pTap[j + 1];
	}
	DataMemBarrier ();
	s_nTapOut = nOut;
	return n;
}

void CAudioSink::SetSounds (CSounds *pSounds, CAudioSink *pSink)
{
	s_pSounds = pSounds;
	s_pSoundsSink = pSink;
}

boolean CAudioSink::HasSounds (void) const
{
	return s_pSoundsSink == this && s_pSounds && s_pSounds->IsLoaded ();
}

CAudioSink::CAudioSink (unsigned nSampleRate)
:	m_nSampleRate (nSampleRate),
	m_pRing (new s16[RingFrames * 2]),
	m_nIn (0),
	m_nOut (0),
	m_nFramesOut (0),
	m_nUnderrun (0),
	m_bPaused (FALSE),
	m_nVolume (65536),
	m_nGain (65536),
	m_nChunkTime (0)
{
	assert (m_pRing);
}

CAudioSink::~CAudioSink (void)
{
	delete [] m_pRing;
}

unsigned CAudioSink::Queued (void) const
{
	return m_nIn - m_nOut;
}

unsigned CAudioSink::Room (void) const
{
	return RingFrames - Queued ();
}

unsigned CAudioSink::Write (const s16 *pFrames, unsigned nFrames)
{
	unsigned nRoom = Room ();
	if (nFrames > nRoom)
	{
		nFrames = nRoom;
	}

	unsigned nIn = m_nIn;
	for (unsigned i = 0; i < nFrames; i++, nIn++)
	{
		unsigned j = (nIn % RingFrames) * 2;
		m_pRing[j] = pFrames[2 * i];
		m_pRing[j + 1] = pFrames[2 * i + 1];
	}
	DataMemBarrier ();			// the frames before the index that shows them
	m_nIn = nIn;

	return nFrames;
}

void CAudioSink::Flush (void)
{
	m_nOut = m_nIn;
}

void CAudioSink::Take (s16 *pBuffer, unsigned nFrames, unsigned *pFromRing, unsigned *pQueued)
{
	m_nChunkTime = CTimer::GetClockTicks ();
	unsigned nAvail = m_nIn - m_nOut;
	DataMemBarrier ();			// the index before the frames it shows
	if (pQueued)
	{
		*pQueued = nAvail;
	}
	unsigned nCopy = m_bPaused ? 0 : nAvail < nFrames ? nAvail : nFrames;

	unsigned nOut = m_nOut;
	for (unsigned i = 0; i < nCopy; i++, nOut++)
	{
		unsigned j = (nOut % RingFrames) * 2;
		pBuffer[2 * i] = m_pRing[j];
		pBuffer[2 * i + 1] = m_pRing[j + 1];
	}
	DataMemBarrier ();			// the frames read before the room is given back
	m_nOut = nOut;

	if (nCopy < nFrames)			// paused, or the ring ran dry: silence, and go on
	{
		memset (pBuffer + 2 * nCopy, 0, (nFrames - nCopy) * 2 * sizeof (s16));
		if (!m_bPaused)
		{
			m_nUnderrun += nFrames - nCopy;
		}
	}

	// the sound effects, over the stream's sound (or the silence)
	unsigned nSound = nCopy;
	if (s_pSoundsSink == this && s_pSounds && s_pSounds->Mix (pBuffer, nFrames, m_nSampleRate))
	{
		nSound = nFrames;
	}

	// a recording's tap: this output's, of one rate from its first frames on
	if (s_bTap && s_pSoundsSink == this && (!s_nTapRate || s_nTapRate == m_nSampleRate))
	{
		if (!s_nTapRate)
		{
			s_nTapStart = CTimer::GetClockTicks64 ();
			s_nTapRate = m_nSampleRate;
		}
		unsigned nIn = s_nTapIn, nRoom = TapFrames - (nIn - s_nTapOut);
		unsigned n = nRoom < nFrames ? nRoom : nFrames;
		s_nTapLost += nFrames - n;
		for (unsigned i = 0; i < n; i++, nIn++)
		{
			unsigned j = (nIn % TapFrames) * 2;
			s_pTap[j] = pBuffer[2 * i];
			s_pTap[j + 1] = pBuffer[2 * i + 1];
		}
		DataMemBarrier ();
		s_nTapIn = nIn;
	}

	// the volume: from the last chunk's to the one asked for across this
	// chunk (a step would click)
	unsigned nVolume = m_nVolume;
	if (nVolume != 65536 || m_nGain != 65536)
	{
		s32 nFrom = (s32) m_nGain, nStep = (s32) nVolume - nFrom;
		for (unsigned i = 0; i < nSound; i++)
		{
			s32 nGain = nFrom + (s32) ((s64) nStep * (s32) (i + 1) / (s32) nFrames);
			pBuffer[2 * i] = (s16) (((s64) pBuffer[2 * i] * nGain) >> 16);
			pBuffer[2 * i + 1] = (s16) (((s64) pBuffer[2 * i + 1] * nGain) >> 16);
		}
		m_nGain = nVolume;
	}
	m_nFramesOut += nFrames;
	if (pFromRing)
	{
		*pFromRing = nCopy;
	}
}

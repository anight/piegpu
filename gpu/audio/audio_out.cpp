//
// audio_out.cpp
//
#include "audio_out.h"
#include <circle/synchronize.h>
#include <circle/util.h>
#include <circle/timer.h>
#include <assert.h>

// a chunk: 2048 frames, 43 ms at 48 kHz (words, 2 a frame). Circle's VCHIQ
// sound keeps two queued in the VideoCore, so the main loop may go that long
// without yielding before it runs dry: with 480-frame chunks (20 ms) it did,
// when a stream opened (the VideoCore's completion flags, SOUNDTEST)
#define CHUNK_WORDS	(2 * CAudioOut::ChunkFrames)

CAudioOut::CAudioOut (CVCHIQDevice *pVCHIQ, unsigned nSampleRate)
:	CVCHIQSoundBaseDevice (pVCHIQ, nSampleRate, CHUNK_WORDS, VCHIQSoundDestinationHDMI),
	m_nSampleRate (nSampleRate),
	m_pRing (new s16[RingFrames * 2]),
	m_nIn (0),
	m_nOut (0),
	m_nFramesOut (0),
	m_nUnderrun (0),
	m_bPaused (FALSE),
	m_nVolume (65536),
	m_nGain (65536),
	m_nChunkTime (0),
	m_nCaptureFrames (CaptureSeconds * nSampleRate),
	m_pCapture (new s16[m_nCaptureFrames * 2]),
	m_nCaptured (0),
	m_pChunks (new TChunk[CaptureChunks]),
	m_nChunks (0),
	m_bCaptureFrozen (FALSE)
{
	assert (m_pRing && m_pCapture && m_pChunks);
}

CAudioOut::~CAudioOut (void)
{
	delete [] m_pChunks;
	delete [] m_pCapture;
	delete [] m_pRing;
}

void CAudioOut::GetCapture (const s16 **pFirst, unsigned *nFirst, const s16 **pSecond, unsigned *nSecond) const
{
	unsigned nFrames = m_nCaptured < m_nCaptureFrames ? m_nCaptured : m_nCaptureFrames;
	unsigned nStart = (m_nCaptured - nFrames) % m_nCaptureFrames;
	unsigned nTail = m_nCaptureFrames - nStart < nFrames ? m_nCaptureFrames - nStart : nFrames;
	*pFirst = m_pCapture + 2 * nStart;
	*nFirst = nTail;
	*pSecond = m_pCapture;
	*nSecond = nFrames - nTail;
}

void CAudioOut::GetChunks (const TChunk **pFirst, unsigned *nFirst, const TChunk **pSecond, unsigned *nSecond) const
{
	unsigned n = m_nChunks < CaptureChunks ? m_nChunks : CaptureChunks;
	unsigned nStart = (m_nChunks - n) % CaptureChunks;
	unsigned nTail = CaptureChunks - nStart < n ? CaptureChunks - nStart : n;
	*pFirst = m_pChunks + nStart;
	*nFirst = nTail;
	*pSecond = m_pChunks;
	*nSecond = n - nTail;
}

unsigned CAudioOut::Queued (void) const
{
	return m_nIn - m_nOut;
}

unsigned CAudioOut::Room (void) const
{
	return RingFrames - Queued ();
}

unsigned CAudioOut::Write (const s16 *pFrames, unsigned nFrames)
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

void CAudioOut::Flush (void)
{
	m_nOut = m_nIn;
}

unsigned CAudioOut::GetChunk (s16 *pBuffer, unsigned nChunkSize)
{
	unsigned nFrames = nChunkSize / 2;
	m_nChunkTime = CTimer::GetClockTicks ();
	if (m_bPaused)
	{
		memset (pBuffer, 0, nChunkSize * sizeof (s16));
		m_nFramesOut += nFrames;
		return nChunkSize;
	}
	unsigned nAvail = m_nIn - m_nOut;
	DataMemBarrier ();			// the index before the frames it shows
	unsigned nCopy = nAvail < nFrames ? nAvail : nFrames;

	unsigned nOut = m_nOut;
	for (unsigned i = 0; i < nCopy; i++, nOut++)
	{
		unsigned j = (nOut % RingFrames) * 2;
		pBuffer[2 * i] = m_pRing[j];
		pBuffer[2 * i + 1] = m_pRing[j + 1];
	}
	DataMemBarrier ();			// the frames read before the room is given back
	m_nOut = nOut;

	if (nCopy < nFrames)			// the ring ran dry: silence, and go on
	{
		memset (pBuffer + 2 * nCopy, 0, (nFrames - nCopy) * 2 * sizeof (s16));
		m_nUnderrun += nFrames - nCopy;
	}

	// the volume: from the last chunk's to the one asked for across this
	// chunk (a step would click)
	unsigned nVolume = m_nVolume;
	if (nVolume != 65536 || m_nGain != 65536)
	{
		s32 nFrom = (s32) m_nGain, nStep = (s32) nVolume - nFrom;
		for (unsigned i = 0; i < nCopy; i++)
		{
			s32 nGain = nFrom + (s32) ((s64) nStep * (s32) (i + 1) / (s32) nFrames);
			pBuffer[2 * i] = (s16) (((s64) pBuffer[2 * i] * nGain) >> 16);
			pBuffer[2 * i + 1] = (s16) (((s64) pBuffer[2 * i + 1] * nGain) >> 16);
		}
		m_nGain = nVolume;
	}
	m_nFramesOut += nFrames;

	if (!m_bCaptureFrozen)
	{
		for (unsigned i = 0; i < nFrames; )
		{
			unsigned nAt = m_nCaptured % m_nCaptureFrames;
			unsigned n = m_nCaptureFrames - nAt < nFrames - i ? m_nCaptureFrames - nAt : nFrames - i;
			memcpy (m_pCapture + 2 * nAt, pBuffer + 2 * i, n * 2 * sizeof (s16));
			m_nCaptured += n;
			i += n;
		}
		TChunk &C = m_pChunks[m_nChunks % CaptureChunks];
		C.nTime = CTimer::GetClockTicks ();
		C.nFrames = (u16) nFrames;
		C.nFromRing = (u16) nCopy;
		C.nQueued = nAvail;
		m_nChunks++;
	}

	return nChunkSize;
}

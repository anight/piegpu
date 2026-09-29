//
// audio_out.cpp
//
#include "audio_out.h"
#include <circle/synchronize.h>
#include <circle/util.h>
#include <assert.h>

// a chunk: about 10 ms at 48 kHz (the VCHIQ service's unit; words, 2 a frame)
#define CHUNK_WORDS	960

CAudioOut::CAudioOut (CVCHIQDevice *pVCHIQ, unsigned nSampleRate)
:	CVCHIQSoundBaseDevice (pVCHIQ, nSampleRate, CHUNK_WORDS, VCHIQSoundDestinationHDMI),
	m_nSampleRate (nSampleRate),
	m_pRing (new s16[RingFrames * 2]),
	m_nIn (0),
	m_nOut (0),
	m_nFramesOut (0),
	m_nUnderrun (0),
	m_bPaused (FALSE)
{
	assert (m_pRing);
}

CAudioOut::~CAudioOut (void)
{
	delete [] m_pRing;
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
	m_nFramesOut += nFrames;

	return nChunkSize;
}

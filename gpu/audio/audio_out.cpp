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

CAudioOut::CDevice::CDevice (CAudioOut *pOwner, CVCHIQDevice *pVCHIQ, unsigned nSampleRate)
:	CVCHIQSoundBaseDevice (pVCHIQ, nSampleRate, CHUNK_WORDS, VCHIQSoundDestinationHDMI),
	m_pOwner (pOwner)
{
}

CAudioOut::CAudioOut (CVCHIQDevice *pVCHIQ, unsigned nSampleRate)
:	CAudioSink (nSampleRate),
	m_Device (this, pVCHIQ, nSampleRate),
	m_nCaptureFrames (CaptureSeconds * nSampleRate),
	m_pCapture (new s16[m_nCaptureFrames * 2]),
	m_nCaptured (0),
	m_pChunks (new TChunk[CaptureChunks]),
	m_nChunks (0),
	m_bCaptureFrozen (FALSE)
{
	assert (m_pCapture && m_pChunks);
}

CAudioOut::~CAudioOut (void)
{
	delete [] m_pChunks;
	delete [] m_pCapture;
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

unsigned CAudioOut::GetChunk (s16 *pBuffer, unsigned nChunkSize)
{
	unsigned nFrames = nChunkSize / 2, nCopy, nAvail;
	Take (pBuffer, nFrames, &nCopy, &nAvail);

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

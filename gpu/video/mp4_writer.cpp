//
// mp4_writer.cpp
//
#include "mp4_writer.h"
#include <circle/logger.h>
#include <circle/timer.h>
#include <circle/util.h>
#include <assert.h>

LOGMODULE ("mp4");

#define HEADER_BYTES	512		// ftyp (32), a free box that fills a sector, mdat's header (8): what
					// follows is written in whole sectors, from a sector's start
#define VIDEO_SCALE	1000000		// the video's times: microseconds
#define KEY_FRAME	0x80000000u

// boxes built in memory (the index): a box's size is filled in when it ends
class CBoxes
{
public:
	CBoxes (void) : m_p (nullptr), m_n (0), m_nMax (0), m_nDepth (0) {}
	~CBoxes (void)				{ delete [] m_p; }

	void Bytes (const void *p, unsigned n)
	{
		if (m_n + n > m_nMax)
		{
			unsigned nMax = (m_n + n) * 2 + 4096;
			u8 *pNew = new u8[nMax];
			if (m_n)
			{
				memcpy (pNew, m_p, m_n);
			}
			delete [] m_p;
			m_p = pNew;
			m_nMax = nMax;
		}
		memcpy (m_p + m_n, p, n);
		m_n += n;
	}
	void U8 (u8 v)				{ Bytes (&v, 1); }
	void U16 (u16 v)			{ u8 b[2] = {(u8) (v >> 8), (u8) v}; Bytes (b, 2); }
	void U32 (u32 v)			{ u8 b[4] = {(u8) (v >> 24), (u8) (v >> 16), (u8) (v >> 8), (u8) v}; Bytes (b, 4); }
	void Zero (unsigned n)			{ while (n--) U8 (0); }
	void Begin (const char *pType)		{ assert (m_nDepth < 12); m_Start[m_nDepth++] = m_n; U32 (0); Bytes (pType, 4); }
	void BeginFull (const char *pType, u32 nVersionFlags = 0)	{ Begin (pType); U32 (nVersionFlags); }
	void End (void)
	{
		unsigned nStart = m_Start[--m_nDepth], nSize = m_n - nStart;
		m_p[nStart] = (u8) (nSize >> 24);
		m_p[nStart + 1] = (u8) (nSize >> 16);
		m_p[nStart + 2] = (u8) (nSize >> 8);
		m_p[nStart + 3] = (u8) nSize;
	}
	const u8 *Get (unsigned *pBytes) const	{ *pBytes = m_n; return m_p; }

private:
	u8 *m_p;
	unsigned m_n, m_nMax;
	unsigned m_Start[12], m_nDepth;
};

CMp4Writer::CMp4Writer (void)
:	m_bOpen (FALSE),
	m_bFailed (FALSE),
	m_pRing (nullptr),
	m_pSizes (nullptr),
	m_pVideoChunks (nullptr),
	m_pSound (nullptr),
	m_pSoundChunks (nullptr)
{
}

CMp4Writer::~CMp4Writer (void)
{
	if (m_bOpen)
	{
		Close ();
	}
	delete [] m_pRing;
	delete [] m_pSound;
}

template <class T> boolean CMp4Writer::Grow (T **ppArray, unsigned *pMax, unsigned nNeeded)
{
	if (nNeeded <= *pMax)
	{
		return TRUE;
	}
	unsigned nMax = nNeeded * 2 + 256;
	T *pNew = new T[nMax];
	if (!pNew)
	{
		return FALSE;
	}
	if (*ppArray)
	{
		memcpy (pNew, *ppArray, *pMax * sizeof (T));
		delete [] *ppArray;
	}
	*ppArray = pNew;
	*pMax = nMax;
	return TRUE;
}

boolean CMp4Writer::Open (const char *pPath, unsigned nWidth, unsigned nHeight)
{
	assert (!m_bOpen);
	if (!m_pRing)
	{
		m_pRing = new u8[RingBytes];
		m_pSound = new s16[ChunkFrames * 2 * 2];
	}
	if (!m_pRing || !m_pSound)
	{
		return FALSE;
	}
	FRESULT Result = f_open (&m_File, pPath, FA_WRITE | FA_CREATE_ALWAYS);
	if (Result != FR_OK)
	{
		LOGWARN ("%s: can't be made (%d)", pPath, (int) Result);
		return FALSE;
	}

	// ftyp, and mdat's header (its size is known at the end)
	static const u8 Start[40] =
	{
		0, 0, 0, 32, 'f', 't', 'y', 'p', 'i', 's', 'o', 'm', 0, 0, 2, 0,
		'i', 's', 'o', 'm', 'i', 's', 'o', '2', 'a', 'v', 'c', '1', 'm', 'p', '4', '1',
		0, 0, (HEADER_BYTES - 40) >> 8, (HEADER_BYTES - 40) & 0xFF, 'f', 'r', 'e', 'e',
	};
	static u8 Header[HEADER_BYTES];
	memset (Header, 0, sizeof Header);
	memcpy (Header, Start, sizeof Start);
	memcpy (Header + HEADER_BYTES - 4, "mdat", 4);
	UINT nDone = 0;
	Result = f_write (&m_File, Header, sizeof Header, &nDone);
	if (Result != FR_OK || nDone != sizeof Header)
	{
		LOGWARN ("%s: can't be written (%d)", pPath, (int) Result);
		f_close (&m_File);
		return FALSE;
	}

	m_nWidth = nWidth;
	m_nHeight = nHeight;
	m_nIn = m_nOut = 0;
	m_nWritten = HEADER_BYTES;
	m_nOffset = HEADER_BYTES;
	m_nSPS = m_nPPS = 0;
	m_nVideo = m_nVideoMax = 0;
	m_nVideoChunks = m_nVideoChunksMax = 0;
	m_nSoundChunks = m_nSoundChunksMax = 0;
	m_pSizes = nullptr;
	m_pVideoChunks = m_pSoundChunks = nullptr;
	m_bInVideoChunk = FALSE;
	m_nSoundWaiting = m_nSoundFrames = m_nRate = 0;
	m_nWrites = m_nLongestUs = m_nMostWaiting = 0;
	m_nWriteUs = 0;
	m_bFailed = FALSE;
	m_bOpen = TRUE;

	return TRUE;
}

void CMp4Writer::Put (const void *pData, unsigned nBytes)
{
	if (m_nIn - m_nOut + nBytes > RingBytes)
	{
		if (!m_bFailed)
		{
			LOGWARN ("The card is too slow: %u KB wait", (unsigned) ((m_nIn - m_nOut) / 1024));
		}
		m_bFailed = TRUE;
		return;
	}
	const u8 *p = (const u8 *) pData;
	unsigned nAt = (unsigned) (m_nIn % RingBytes), nTail = RingBytes - nAt;
	nTail = nTail < nBytes ? nTail : nBytes;
	memcpy (m_pRing + nAt, p, nTail);
	memcpy (m_pRing, p + nTail, nBytes - nTail);
	m_nIn += nBytes;
	m_nOffset += nBytes;
	unsigned nWaiting = (unsigned) (m_nIn - m_nOut);
	m_nMostWaiting = nWaiting > m_nMostWaiting ? nWaiting : m_nMostWaiting;
}

void CMp4Writer::Put32 (u32 nValue)
{
	const u8 b[4] = {(u8) (nValue >> 24), (u8) (nValue >> 16), (u8) (nValue >> 8), (u8) nValue};
	Put (b, 4);
}

// the next NAL unit of an Annex B stream from pAt on: where it starts and
// how long it is; nullptr if there's none
static const u8 *NextNAL (const u8 *pAt, const u8 *pEnd, unsigned *pBytes, const u8 **ppNext)
{
	const u8 *p = pAt;
	while (p + 3 <= pEnd && !(p[0] == 0 && p[1] == 0 && p[2] == 1))
	{
		p++;
	}
	if (p + 3 > pEnd)
	{
		return nullptr;
	}
	const u8 *pStart = p + 3, *q = pStart;
	while (q + 3 <= pEnd && !(q[0] == 0 && q[1] == 0 && (q[2] == 1 || (q[2] == 0 && q + 4 <= pEnd && q[3] == 1))))
	{
		q++;
	}
	if (q + 3 > pEnd)
	{
		q = pEnd;
	}
	*pBytes = (unsigned) (q - pStart);
	*ppNext = q;
	return pStart;
}

void CMp4Writer::Config (const u8 *pData, unsigned nBytes)
{
	const u8 *pEnd = pData + nBytes, *pNAL;
	unsigned n;
	while ((pNAL = NextNAL (pData, pEnd, &n, &pData)) != nullptr)
	{
		if (n && (pNAL[0] & 0x1F) == 7 && !m_nSPS && n <= sizeof m_SPS)
		{
			memcpy (m_SPS, pNAL, m_nSPS = n);
		}
		else if (n && (pNAL[0] & 0x1F) == 8 && !m_nPPS && n <= sizeof m_PPS)
		{
			memcpy (m_PPS, pNAL, m_nPPS = n);
		}
	}
}

// the frame's NAL units, each behind its length; the parameter sets (they
// are in the index) and the delimiters left out
void CMp4Writer::Video (const u8 *pData, unsigned nBytes, boolean bKey, u64 nTimeUs)
{
	if (!m_bOpen || m_bFailed)
	{
		return;
	}
	if (!Grow (&m_pSizes, &m_nVideoMax, 2 * (m_nVideo + 1)))
	{
		m_bFailed = TRUE;
		return;
	}
	u32 nStart = m_nOffset;
	const u8 *pEnd = pData + nBytes, *pNAL;
	unsigned n;
	while ((pNAL = NextNAL (pData, pEnd, &n, &pData)) != nullptr)
	{
		unsigned nType = n ? pNAL[0] & 0x1F : 0;
		if (nType == 7 || nType == 8)
		{
			Config (pNAL - 3, n + 3);
		}
		else if (n && nType != 9)
		{
			Put32 (n);
			Put (pNAL, n);
		}
	}
	if (m_nOffset == nStart || m_bFailed)
	{
		return;				// nothing of a picture in it
	}

	if (!m_nVideo)
	{
		m_nFirstTime = nTimeUs;
	}
	if (!m_bInVideoChunk)
	{
		if (!Grow (&m_pVideoChunks, &m_nVideoChunksMax, m_nVideoChunks + 1))
		{
			m_bFailed = TRUE;
			return;
		}
		m_pVideoChunks[m_nVideoChunks].nOffset = nStart;
		m_pVideoChunks[m_nVideoChunks++].nSamples = 0;
		m_bInVideoChunk = TRUE;
	}
	m_pVideoChunks[m_nVideoChunks - 1].nSamples++;
	m_pSizes[2 * m_nVideo] = (m_nOffset - nStart) | (bKey ? KEY_FRAME : 0);
	m_pSizes[2 * m_nVideo + 1] = (u32) (nTimeUs - m_nFirstTime);
	m_nVideo++;
}

void CMp4Writer::CutSound (void)
{
	if (!m_nSoundWaiting)
	{
		return;
	}
	if (!Grow (&m_pSoundChunks, &m_nSoundChunksMax, m_nSoundChunks + 1))
	{
		m_bFailed = TRUE;
		return;
	}
	m_pSoundChunks[m_nSoundChunks].nOffset = m_nOffset;
	m_pSoundChunks[m_nSoundChunks++].nSamples = m_nSoundWaiting;
	Put (m_pSound, m_nSoundWaiting * 4);
	m_nSoundFrames += m_nSoundWaiting;
	m_nSoundWaiting = 0;
	m_bInVideoChunk = FALSE;		// the video goes on in a chunk after it
}

void CMp4Writer::Sound (const s16 *pFrames, unsigned nFrames, unsigned nRate)
{
	if (!m_bOpen || m_bFailed || (m_nRate && nRate != m_nRate))
	{
		return;
	}
	m_nRate = nRate;
	while (nFrames)
	{
		unsigned n = 2 * ChunkFrames - m_nSoundWaiting;
		n = n < nFrames ? n : nFrames;
		memcpy (m_pSound + 2 * m_nSoundWaiting, pFrames, n * 4);
		m_nSoundWaiting += n;
		pFrames += 2 * n;
		nFrames -= n;
		if (m_nSoundWaiting >= nRate / 2)
		{
			CutSound ();
		}
	}
}

boolean CMp4Writer::WriteBlock (unsigned nBytes)
{
	unsigned nAt = (unsigned) (m_nOut % RingBytes);
	nBytes = RingBytes - nAt < nBytes ? RingBytes - nAt : nBytes;	// (to the ring's end: the rest next time)
	unsigned nStart = CTimer::GetClockTicks ();
	UINT nDone = 0;
	FRESULT Result = f_write (&m_File, m_pRing + nAt, nBytes, &nDone);
	unsigned nUs = CTimer::GetClockTicks () - nStart;
	m_nWrites++;
	m_nWriteUs += nUs;
	m_nLongestUs = nUs > m_nLongestUs ? nUs : m_nLongestUs;
	if (Result != FR_OK || nDone != nBytes)
	{
		LOGWARN ("The card took %u of %u bytes (%d): full?", (unsigned) nDone, nBytes, (int) Result);
		m_bFailed = TRUE;
		return FALSE;
	}
	m_nOut += nBytes;
	m_nWritten += nBytes;
	return TRUE;
}

void CMp4Writer::Work (void)
{
	if (m_bOpen && !m_bFailed && m_nIn - m_nOut >= BlockBytes)
	{
		WriteBlock (BlockBytes);
	}
}

void CMp4Writer::GetWrites (unsigned *pCount, u64 *pUs, unsigned *pLongestUs, unsigned *pMostWaiting) const
{
	*pCount = m_nWrites;
	*pUs = m_nWriteUs;
	*pLongestUs = m_nLongestUs;
	*pMostWaiting = m_nMostWaiting;
}

// moov: the movie's header and its two tracks
void CMp4Writer::BuildIndex (CBoxes &B)
{
	static const u32 Matrix[9] = {0x10000, 0, 0, 0, 0x10000, 0, 0, 0, 0x40000000};
	// the frames' durations: to the next one's time; the last as the one before
	u32 nVideoUs = 0;
	for (unsigned i = 0; i < m_nVideo; i++)
	{
		u32 nTime = m_pSizes[2 * i + 1];
		u32 nDuration =   i + 1 < m_nVideo ? m_pSizes[2 * i + 3] - nTime
				: i ? m_pSizes[2 * i - 1] : VIDEO_SCALE / 60;	// (the one before: a duration by now)
		m_pSizes[2 * i + 1] = nDuration ? nDuration : 1;
		nVideoUs += m_pSizes[2 * i + 1];
	}
	u32 nVideoMs = nVideoUs / 1000;
	u32 nSoundMs = m_nRate ? (u32) ((u64) m_nSoundFrames * 1000 / m_nRate) : 0;

	B.Begin ("moov");
	B.BeginFull ("mvhd");
	B.U32 (0); B.U32 (0);				// made, changed
	B.U32 (1000);					// the movie's times: milliseconds
	B.U32 (nVideoMs > nSoundMs ? nVideoMs : nSoundMs);
	B.U32 (0x10000); B.U16 (0x100); B.Zero (10);	// rate 1, volume 1
	for (unsigned i = 0; i < 9; i++) B.U32 (Matrix[i]);
	B.Zero (24);
	B.U32 (3);					// the next track's id
	B.End ();

	for (unsigned nTrack = 1; nTrack <= (m_nSoundFrames ? 2u : 1u); nTrack++)
	{
		boolean bVideo = nTrack == 1;
		const TChunk *pChunks = bVideo ? m_pVideoChunks : m_pSoundChunks;
		unsigned nChunks = bVideo ? m_nVideoChunks : m_nSoundChunks;

		B.Begin ("trak");
		B.BeginFull ("tkhd", 3);			// enabled, in the movie
		B.U32 (0); B.U32 (0); B.U32 (nTrack); B.U32 (0);
		B.U32 (bVideo ? nVideoMs : nSoundMs);
		B.Zero (8); B.U16 (0); B.U16 (0);		// layer, alternate group
		B.U16 (bVideo ? 0 : 0x100); B.U16 (0);		// volume
		for (unsigned i = 0; i < 9; i++) B.U32 (Matrix[i]);
		B.U32 (bVideo ? m_nWidth << 16 : 0); B.U32 (bVideo ? m_nHeight << 16 : 0);
		B.End ();

		B.Begin ("mdia");
		B.BeginFull ("mdhd");
		B.U32 (0); B.U32 (0);
		B.U32 (bVideo ? VIDEO_SCALE : m_nRate);
		B.U32 (bVideo ? nVideoUs : m_nSoundFrames);
		B.U16 (0x55C4); B.U16 (0);			// language: undetermined
		B.End ();
		B.BeginFull ("hdlr");
		B.U32 (0); B.Bytes (bVideo ? "vide" : "soun", 4); B.Zero (12);
		B.Bytes (bVideo ? "piegpu video" : "piegpu sound", 13);
		B.End ();

		B.Begin ("minf");
		if (bVideo)
		{
			B.BeginFull ("vmhd", 1); B.Zero (8); B.End ();
		}
		else
		{
			B.BeginFull ("smhd"); B.Zero (4); B.End ();
		}
		B.Begin ("dinf");
		B.BeginFull ("dref"); B.U32 (1);
		B.BeginFull ("url ", 1); B.End ();		// in this file
		B.End ();
		B.End ();

		B.Begin ("stbl");
		B.BeginFull ("stsd"); B.U32 (1);
		if (bVideo)
		{
			B.Begin ("avc1");
			B.Zero (6); B.U16 (1);			// the data reference
			B.Zero (16);
			B.U16 (m_nWidth); B.U16 (m_nHeight);
			B.U32 (0x480000); B.U32 (0x480000);	// 72 dpi
			B.U32 (0); B.U16 (1);			// a frame a sample
			B.Zero (32);				// the compressor's name
			B.U16 (0x18); B.U16 (0xFFFF);
			B.Begin ("avcC");
			B.U8 (1); B.U8 (m_SPS[1]); B.U8 (m_SPS[2]); B.U8 (m_SPS[3]);
			B.U8 (0xFF);				// lengths of 4 bytes
			B.U8 (0xE1); B.U16 (m_nSPS); B.Bytes (m_SPS, m_nSPS);
			B.U8 (1); B.U16 (m_nPPS); B.Bytes (m_PPS, m_nPPS);
			if (m_SPS[1] == 100 || m_SPS[1] == 110 || m_SPS[1] == 122 || m_SPS[1] == 144)
			{
				B.U8 (0xFD); B.U8 (0xF8); B.U8 (0xF8); B.U8 (0);	// 4:2:0, 8 bits
			}
			B.End ();
			B.End ();
		}
		else
		{
			B.Begin ("sowt");			// 16-bit PCM, little endian
			B.Zero (6); B.U16 (1);
			B.Zero (8);
			B.U16 (2); B.U16 (16);			// channels, bits
			B.U32 (0);
			B.U32 (m_nRate << 16);
			B.End ();
		}
		B.End ();

		B.BeginFull ("stts");				// the samples' durations, runs of equal ones
		if (bVideo)
		{
			unsigned nRuns = 0;
			for (unsigned i = 0; i < m_nVideo; i++)
			{
				nRuns += !i || m_pSizes[2 * i + 1] != m_pSizes[2 * i - 1];
			}
			B.U32 (nRuns);
			for (unsigned i = 0; i < m_nVideo; )
			{
				unsigned n = 1;
				while (i + n < m_nVideo && m_pSizes[2 * (i + n) + 1] == m_pSizes[2 * i + 1])
				{
					n++;
				}
				B.U32 (n); B.U32 (m_pSizes[2 * i + 1]);
				i += n;
			}
		}
		else
		{
			B.U32 (1); B.U32 (m_nSoundFrames); B.U32 (1);
		}
		B.End ();

		if (bVideo)
		{
			unsigned nKeys = 0;
			for (unsigned i = 0; i < m_nVideo; i++)
			{
				nKeys += !!(m_pSizes[2 * i] & KEY_FRAME);
			}
			B.BeginFull ("stss"); B.U32 (nKeys);
			for (unsigned i = 0; i < m_nVideo; i++)
			{
				if (m_pSizes[2 * i] & KEY_FRAME)
				{
					B.U32 (i + 1);
				}
			}
			B.End ();
		}

		B.BeginFull ("stsc");				// samples a chunk: runs of chunks with as many
		unsigned nRuns = 0;
		for (unsigned i = 0; i < nChunks; i++)
		{
			nRuns += !i || pChunks[i].nSamples != pChunks[i - 1].nSamples;
		}
		B.U32 (nRuns);
		for (unsigned i = 0; i < nChunks; i++)
		{
			if (!i || pChunks[i].nSamples != pChunks[i - 1].nSamples)
			{
				B.U32 (i + 1); B.U32 (pChunks[i].nSamples); B.U32 (1);
			}
		}
		B.End ();

		B.BeginFull ("stsz");
		if (bVideo)
		{
			B.U32 (0); B.U32 (m_nVideo);
			for (unsigned i = 0; i < m_nVideo; i++)
			{
				B.U32 (m_pSizes[2 * i] & ~KEY_FRAME);
			}
		}
		else
		{
			B.U32 (4); B.U32 (m_nSoundFrames);	// every sample 4 bytes: a frame
		}
		B.End ();

		B.BeginFull ("stco"); B.U32 (nChunks);
		for (unsigned i = 0; i < nChunks; i++)
		{
			B.U32 (pChunks[i].nOffset);
		}
		B.End ();

		B.End ();	// stbl
		B.End ();	// minf
		B.End ();	// mdia
		B.End ();	// trak
	}
	B.End ();		// moov
}

boolean CMp4Writer::Close (void)
{
	if (!m_bOpen)
	{
		return FALSE;
	}
	m_bOpen = FALSE;
	boolean bGood = !m_bFailed && m_nVideo && m_nSPS && m_nPPS;

	if (bGood)
	{
		CutSound ();
		while (!m_bFailed && m_nIn > m_nOut)
		{
			WriteBlock ((unsigned) (m_nIn - m_nOut < BlockBytes ? m_nIn - m_nOut : BlockBytes));
		}
		bGood = !m_bFailed;
	}
	if (bGood)
	{
		CBoxes Boxes;
		BuildIndex (Boxes);
		unsigned nBytes;
		const u8 *pIndex = Boxes.Get (&nBytes);
		u32 nData = (u32) (m_nWritten - HEADER_BYTES + 8);		// mdat's size, with its header
		const u8 Size[4] = {(u8) (nData >> 24), (u8) (nData >> 16), (u8) (nData >> 8), (u8) nData};
		UINT nDone = 0, nDone2 = 0;
		bGood =    f_write (&m_File, pIndex, nBytes, &nDone) == FR_OK && nDone == nBytes
			&& f_lseek (&m_File, HEADER_BYTES - 8) == FR_OK
			&& f_write (&m_File, Size, 4, &nDone2) == FR_OK && nDone2 == 4;
		m_nWritten += nBytes;
	}
	bGood = f_close (&m_File) == FR_OK && bGood;

	delete [] m_pSizes;
	delete [] m_pVideoChunks;
	delete [] m_pSoundChunks;
	m_pSizes = nullptr;
	m_pVideoChunks = m_pSoundChunks = nullptr;

	return bGood;
}

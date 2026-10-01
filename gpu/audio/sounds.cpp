//
// sounds.cpp
//
#include "sounds.h"
#include <circle/util.h>

CSounds::CSounds (void)
:	m_nLoaded (0),
	m_nFrames (0)
{
	memset (m_Sound, 0, sizeof m_Sound);
	memset (m_Channel, 0, sizeof m_Channel);
}

void CSounds::Reset (void)
{
	Delete (0);
}

u32 CSounds::Delete (unsigned nId)
{
	if (nId > MaxSounds)
	{
		return PGPU_ERR_ID;
	}
	for (unsigned i = nId ? nId - 1 : 0; i < (nId ? nId : MaxSounds); i++)
	{
		TSound &S = m_Sound[i];
		if (!S.pFrames)
		{
			continue;
		}
		for (unsigned k = 0; k < Channels; k++)		// (what plays it stops)
		{
			if (m_Channel[k].pSound == &S)
			{
				m_Channel[k].pSound = nullptr;
			}
		}
		delete [] S.pFrames;
		S.pFrames = nullptr;
		m_nFrames -= S.nFrames;
		m_nLoaded--;
	}
	return 0;
}

u32 CSounds::Data (unsigned nId, unsigned nRate, unsigned nFrames, unsigned nOffset, u32 nFormat,
		   const u8 *pData, unsigned nBytes)
{
	if (nId < 1 || nId > MaxSounds)
	{
		return PGPU_ERR_ID;
	}
	unsigned nSampleBytes = nFormat == PGPU_SOUND_S16 ? 2 : 1;
	if (nFormat > PGPU_SOUND_S16 || nRate < 4000 || nRate > 48000 || nFrames < 1)
	{
		return PGPU_ERR_ENUM;
	}
	TSound &S = m_Sound[nId - 1];
	if (nOffset == 0)				// its first part: made anew
	{
		Delete (nId);
		if (nFrames > MaxFrames - m_nFrames)
		{
			return PGPU_ERR_LIMIT;
		}
		S.pFrames = new s16[nFrames + 1];	// (one more: the frame after the last, for the mixing)
		if (!S.pFrames)
		{
			return PGPU_ERR_LIMIT;
		}
		memset (S.pFrames, 0, (nFrames + 1) * sizeof (s16));
		S.nFrames = nFrames;
		S.nRate = nRate;
		m_nFrames += nFrames;
		m_nLoaded++;
	}
	unsigned nCount = nBytes / nSampleBytes;
	if (!S.pFrames || nFrames != S.nFrames || nOffset > S.nFrames || nCount > S.nFrames - nOffset)
	{
		return PGPU_ERR_OBJECT;
	}
	for (unsigned i = 0; i < nCount; i++)		// 8 bits unsigned, or 16 signed: kept as 16
	{
		S.pFrames[nOffset + i] = nSampleBytes == 1 ? (s16) ((pData[i] - 128) << 8)
							   : (s16) (pData[2 * i] | pData[2 * i + 1] << 8);
	}
	S.pFrames[S.nFrames] = S.pFrames[0];		// (a loop's: the first again)
	return 0;
}

u32 CSounds::Play (unsigned nChannel, unsigned nId, unsigned nLeft, unsigned nRight, u32 nFlags)
{
	if (nChannel >= Channels || nId > MaxSounds)
	{
		return PGPU_ERR_ID;
	}
	TChannel &C = m_Channel[nChannel];
	if (nId == 0)
	{
		if (C.pSound && C.bLoop)		// (a loop cut where it is would click: faded out)
		{
			C.nLeft = C.nRight = 0;
			C.bStopping = TRUE;
		}
		else
		{
			C.pSound = nullptr;
		}
		return 0;
	}
	if (!m_Sound[nId - 1].pFrames)
	{
		return PGPU_ERR_OBJECT;
	}
	C.pSound = &m_Sound[nId - 1];
	C.nAt = 0;
	C.bLoop = !!(nFlags & PGPU_SOUND_LOOP);
	C.bStopping = FALSE;
	C.nLeft = nLeft > 256 ? 256 : (int) nLeft;
	C.nRight = nRight > 256 ? 256 : (int) nRight;
	C.nNowLeft = C.bLoop ? 0 : C.nLeft;		// (a loop starts anywhere in its wave: faded in)
	C.nNowRight = C.bLoop ? 0 : C.nRight;
	return 0;
}

u32 CSounds::Volume (unsigned nChannel, unsigned nLeft, unsigned nRight)
{
	if (nChannel >= Channels)
	{
		return PGPU_ERR_ID;
	}
	if (!m_Channel[nChannel].bStopping)
	{
		m_Channel[nChannel].nLeft = nLeft > 256 ? 256 : (int) nLeft;
		m_Channel[nChannel].nRight = nRight > 256 ? 256 : (int) nRight;
	}
	return 0;
}

boolean CSounds::Mix (s16 *pFrames, unsigned nFrames, unsigned nRate)
{
	boolean bMixed = FALSE;
	for (unsigned k = 0; k < Channels; k++)
	{
		TChannel &C = m_Channel[k];
		const TSound *pSound = C.pSound;
		if (!pSound)
		{
			continue;
		}
		bMixed = TRUE;
		u64 nStep = ((u64) pSound->nRate << 16) / nRate, nEnd = (u64) pSound->nFrames << 16;
		int nLeft = C.nNowLeft, nRight = C.nNowRight;
		for (unsigned i = 0; i < nFrames; i++)
		{
			if (C.nAt >= nEnd)
			{
				if (!C.bLoop)
				{
					C.pSound = nullptr;
					break;
				}
				C.nAt %= nEnd;
			}
			// between two of its frames; the volumes eased to the ones asked for
			unsigned nIndex = (unsigned) (C.nAt >> 16);
			int nPart = (int) (C.nAt & 0xFFFF);
			int a = pSound->pFrames[nIndex], b = pSound->pFrames[nIndex + 1];
			if (nIndex + 1 == pSound->nFrames && !C.bLoop)
			{
				b = 0;
			}
			int nSample = a + ((b - a) * nPart >> 16);
			if ((i & 7) == 0)		// (all the way in 2048 frames: 43 ms at 48 kHz)
			{
				nLeft += nLeft < C.nLeft ? 1 : nLeft > C.nLeft ? -1 : 0;
				nRight += nRight < C.nRight ? 1 : nRight > C.nRight ? -1 : 0;
			}
			int l = pFrames[2 * i] + (nSample * nLeft >> 8), r = pFrames[2 * i + 1] + (nSample * nRight >> 8);
			pFrames[2 * i] = (s16) (l > 32767 ? 32767 : l < -32768 ? -32768 : l);
			pFrames[2 * i + 1] = (s16) (r > 32767 ? 32767 : r < -32768 ? -32768 : r);
			C.nAt += nStep;
		}
		C.nNowLeft = nLeft;
		C.nNowRight = nRight;
		if (C.bStopping && nLeft == 0 && nRight == 0)
		{
			C.pSound = nullptr;
		}
	}
	return bMixed;
}

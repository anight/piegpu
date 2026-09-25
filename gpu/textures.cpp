//
// textures.cpp
//
#include "textures.h"
#include <v3d.h>
#include <pgpu_protocol.h>
#include <circle/util.h>
#include <assert.h>

// texture formats (docs/protocol.md 10.6)
enum
{
	FORMAT_RGBA8888, FORMAT_RGB565, FORMAT_RGBA4444, FORMAT_RGBA5551,
	FORMAT_L8, FORMAT_A8, FORMAT_LA88, FORMAT_ETC1, FORMATS
};

static const unsigned BytesPerPixel[FORMATS] = {4, 2, 2, 2, 1, 1, 2, 0};

// TMU config (VideoCore IV 3D reference guide, texture config parameters)
#define TEX_TYPE_RGBA32R	16		// raster RGBA8888: bits 3:0 in P0, bit 4 in P1
#define P0_TYPE__SHIFT		4
#define P1_TYPE4		(1U << 31)
#define P1_HEIGHT__SHIFT	20
#define P1_WIDTH__SHIFT		8
#define P1_MAGFILT_NEAREST	(1 << 7)
#define P1_MINFILT__SHIFT	4		// 0 linear, 1 nearest
#define P1_WRAP_T__SHIFT	2
#define P1_WRAP_S__SHIFT	0		// 0 repeat, 1 clamp, 2 mirror

#define RGBA(r, g, b, a)	((u32) (r) | (u32) (g) << 8 | (u32) (b) << 16 | (u32) (a) << 24)

CTextures::CTextures (void)
:	m_nTotalBytes (0),
	m_nRetired (0)
{
	memset (m_Textures, 0, sizeof m_Textures);
}

CTextures::~CTextures (void)
{
}

void CTextures::Reset (void)
{
	for (unsigned i = 1; i <= MaxTextures; i++)
	{
		if (m_Textures[i].bValid)
		{
			Delete (i);
		}
	}
}

boolean CTextures::Alloc (TTexture *pTexture, TStorage *pStorage)
{
	unsigned nBytes = pTexture->nStride * pTexture->nHeight * 4;
	if (m_nTotalBytes + nBytes > MaxTotalBytes)
	{
		return FALSE;
	}

	pStorage->pRaw = new u8[nBytes + 4096];
	if (!pStorage->pRaw)
	{
		return FALSE;
	}

	pStorage->pPixels = (u32 *) (((uintptr) pStorage->pRaw + 4095) & ~(uintptr) 4095);
	m_nTotalBytes += nBytes;

	return TRUE;
}

void CTextures::Free (TStorage *pStorage)
{
	delete [] pStorage->pRaw;
	pStorage->pRaw = nullptr;
	pStorage->pPixels = nullptr;
}

// storage still referenced by the current frame: free it after the frame
void CTextures::Retire (TStorage *pStorage)
{
	if (m_nRetired < MaxRetired)
	{
		m_Retired[m_nRetired++] = *pStorage;
	}
	else
	{
		// too many replacements in one frame: leak rather than corrupt
	}

	pStorage->pRaw = nullptr;
	pStorage->pPixels = nullptr;
}

u32 CTextures::Create (u32 nId, unsigned nWidth, unsigned nHeight, u32 nFormat)
{
	if (nId < 1 || nId > MaxTextures)
	{
		return PGPU_ERR_ID;
	}

	if (   nWidth < 1 || nWidth > MaxSize || (nWidth & (nWidth - 1))
	    || nHeight < 1 || nHeight > MaxSize || (nHeight & (nHeight - 1))
	    || nFormat >= FORMATS)
	{
		return PGPU_ERR_ENUM;
	}

	if (m_Textures[nId].bValid)
	{
		Delete (nId);
	}

	TTexture &T = m_Textures[nId];
	T.nWidth = nWidth;
	T.nHeight = nHeight;
	T.nStride = nWidth < 4 ? 4 : nWidth;	// raster rows: at least 16 bytes (seen with 2x2, 8x8)
	T.nFormat = nFormat;
	T.bUsed = FALSE;
	T.nP1Filters = P1_MAGFILT_NEAREST | (1 << P1_MINFILT__SHIFT);	// nearest, repeat

	if (!Alloc (&T, &T.Storage))
	{
		return PGPU_ERR_MEMORY;
	}

	memset (T.Storage.pPixels, 0, T.nStride * T.nHeight * 4);
	CV3D::Flush (T.Storage.pPixels, T.nStride * T.nHeight * 4);
	T.bValid = TRUE;

	return 0;
}

u32 CTextures::Data (u32 nId, unsigned x, unsigned y, unsigned nWidth, unsigned nHeight,
		     const u32 *pData, unsigned nWords)
{
	if (nId < 1 || nId > MaxTextures || !m_Textures[nId].bValid)
	{
		return PGPU_ERR_OBJECT;
	}

	TTexture &T = m_Textures[nId];
	if (   x + nWidth > T.nWidth || y + nHeight > T.nHeight
	    || nWidth == 0 || nHeight == 0)
	{
		return PGPU_ERR_LIMIT;
	}

	// copy-on-write: a draw of this frame still samples the old contents
	if (T.bUsed)
	{
		TStorage New;
		if (!Alloc (&T, &New))
		{
			return PGPU_ERR_MEMORY;
		}
		memcpy (New.pPixels, T.Storage.pPixels, T.nStride * T.nHeight * 4);
		m_nTotalBytes -= T.nStride * T.nHeight * 4;	// the old storage is freed after the frame
		Retire (&T.Storage);
		T.Storage = New;
		T.bUsed = FALSE;
	}

	const u8 *pBytes = (const u8 *) pData;
	unsigned nBytes = nWords * 4;

	if (T.nFormat == FORMAT_ETC1)
	{
		if ((x | y | nWidth | nHeight) & 3)
		{
			return PGPU_ERR_LIMIT;
		}

		unsigned nBlocks = (nWidth / 4) * (nHeight / 4);
		if (nBlocks * 8 > nBytes)
		{
			return PGPU_ERR_LENGTH;
		}

		for (unsigned by = 0; by < nHeight / 4; by++)
		{
			for (unsigned bx = 0; bx < nWidth / 4; bx++, pBytes += 8)
			{
				u32 Out[16];
				DecodeETC1 (pBytes, Out);
				for (unsigned py = 0; py < 4; py++)
				{
					// ETC1 blocks are stored top row first; GL rows go bottom-up
					u32 *pRow = T.Storage.pPixels + (y + by * 4 + py) * T.nStride + x + bx * 4;
					for (unsigned px = 0; px < 4; px++)
					{
						pRow[px] = Out[py * 4 + px];
					}
				}
			}
		}
	}
	else
	{
		unsigned nBPP = BytesPerPixel[T.nFormat];
		unsigned nRowBytes = (nWidth * nBPP + 3) & ~3;		// rows padded to 4 bytes
		if (nRowBytes * nHeight > nBytes)
		{
			return PGPU_ERR_LENGTH;
		}

		for (unsigned row = 0; row < nHeight; row++)
		{
			const u8 *s = pBytes + row * nRowBytes;
			u32 *d = T.Storage.pPixels + (y + row) * T.nStride + x;	// row 0 = t 0

			for (unsigned i = 0; i < nWidth; i++)
			{
				u32 nPixel;
				switch (T.nFormat)
				{
				case FORMAT_RGBA8888:
					nPixel = RGBA (s[0], s[1], s[2], s[3]);
					s += 4;
					break;

				case FORMAT_RGB565: {		// GL_UNSIGNED_SHORT_5_6_5, R in the high bits
					u32 v = s[0] | s[1] << 8;
					u32 r = (v >> 11) & 0x1F, g = (v >> 5) & 0x3F, b = v & 0x1F;
					nPixel = RGBA (r << 3 | r >> 2, g << 2 | g >> 4, b << 3 | b >> 2, 255);
					s += 2;
					} break;

				case FORMAT_RGBA4444: {
					u32 v = s[0] | s[1] << 8;
					nPixel = RGBA (((v >> 12) & 15) * 17, ((v >> 8) & 15) * 17,
						       ((v >> 4) & 15) * 17, (v & 15) * 17);
					s += 2;
					} break;

				case FORMAT_RGBA5551: {
					u32 v = s[0] | s[1] << 8;
					u32 r = (v >> 11) & 0x1F, g = (v >> 6) & 0x1F, b = (v >> 1) & 0x1F;
					nPixel = RGBA (r << 3 | r >> 2, g << 3 | g >> 2, b << 3 | b >> 2,
						       v & 1 ? 255 : 0);
					s += 2;
					} break;

				case FORMAT_L8:
					nPixel = RGBA (s[0], s[0], s[0], 255);
					s += 1;
					break;

				case FORMAT_A8:			// MODULATE: C = Cf, A = Af * At
					nPixel = RGBA (255, 255, 255, s[0]);
					s += 1;
					break;

				default:			// FORMAT_LA88
					nPixel = RGBA (s[0], s[0], s[0], s[1]);
					s += 2;
					break;
				}

				*d++ = nPixel;
			}
		}
	}

	CV3D::Flush (T.Storage.pPixels, T.nStride * T.nHeight * 4);

	return 0;
}

u32 CTextures::Params (u32 nId, u32 nMinFilter, u32 nMagFilter, u32 nWrapS, u32 nWrapT)
{
	if (nId < 1 || nId > MaxTextures || !m_Textures[nId].bValid)
	{
		return PGPU_ERR_OBJECT;
	}

	if (nMinFilter > 5 || nMagFilter > 1 || nWrapS > 2 || nWrapT > 2)
	{
		return PGPU_ERR_ENUM;
	}

	// mipmap filters behave like their base filter (v1): NEAREST_* even, LINEAR_* odd
	boolean bMinNearest = nMinFilter == 0 || nMinFilter == 2 || nMinFilter == 4;
	boolean bMagNearest = nMagFilter == 0;

	m_Textures[nId].nP1Filters =   (bMagNearest ? P1_MAGFILT_NEAREST : 0)
				     | ((bMinNearest ? 1 : 0) << P1_MINFILT__SHIFT)
				     | (nWrapT << P1_WRAP_T__SHIFT)
				     | (nWrapS << P1_WRAP_S__SHIFT);

	return 0;
}

u32 CTextures::Delete (u32 nId)
{
	if (nId < 1 || nId > MaxTextures || !m_Textures[nId].bValid)
	{
		return PGPU_ERR_OBJECT;
	}

	TTexture &T = m_Textures[nId];
	m_nTotalBytes -= T.nStride * T.nHeight * 4;
	if (T.bUsed)
	{
		Retire (&T.Storage);
	}
	else
	{
		Free (&T.Storage);
	}
	T.bValid = FALSE;

	return 0;
}

boolean CTextures::Use (u32 nId, u32 *pP0, u32 *pP1)
{
	if (nId < 1 || nId > MaxTextures || !m_Textures[nId].bValid)
	{
		return FALSE;
	}

	TTexture &T = m_Textures[nId];
	T.bUsed = TRUE;

	*pP0 =   CV3D::BusAddress (T.Storage.pPixels)		// 4 KB aligned
	       | ((TEX_TYPE_RGBA32R & 15) << P0_TYPE__SHIFT);
	*pP1 =   P1_TYPE4					// type bit 4 (RGBA32R)
	       | ((T.nHeight & 0x7FF) << P1_HEIGHT__SHIFT)	// 2048 would be 0
	       | ((T.nWidth & 0x7FF) << P1_WIDTH__SHIFT)
	       | T.nP1Filters;

	return TRUE;
}

void CTextures::EndFrame (void)
{
	for (unsigned i = 0; i < m_nRetired; i++)
	{
		Free (&m_Retired[i]);
	}
	m_nRetired = 0;

	for (unsigned i = 1; i <= MaxTextures; i++)
	{
		m_Textures[i].bUsed = FALSE;
	}
}

// ETC1 (Ericsson texture compression), one 4x4 block of 8 bytes, big endian
void CTextures::DecodeETC1 (const u8 *pBlock, u32 Out[16])
{
	static const int Modifiers[8][4] =
	{
		{2, 8, -2, -8}, {5, 17, -5, -17}, {9, 29, -9, -29}, {13, 42, -13, -42},
		{18, 60, -18, -60}, {24, 80, -24, -80}, {33, 106, -33, -106}, {47, 183, -47, -183}
	};

	u32 nHigh = pBlock[0] << 24 | pBlock[1] << 16 | pBlock[2] << 8 | pBlock[3];
	u32 nLow  = pBlock[4] << 24 | pBlock[5] << 16 | pBlock[6] << 8 | pBlock[7];

	boolean bDiff = (nHigh >> 1) & 1;
	boolean bFlip = nHigh & 1;
	unsigned nTable[2] = {(nHigh >> 5) & 7, (nHigh >> 2) & 7};

	int Base[2][3];
	for (unsigned c = 0; c < 3; c++)
	{
		unsigned nShift = 24 - 8 * c;		// R: bits 31.., G: 23.., B: 15..
		if (bDiff)
		{
			int nBase = (nHigh >> (nShift + 3)) & 0x1F;
			int nDelta = (nHigh >> nShift) & 7;
			if (nDelta & 4)
			{
				nDelta -= 8;
			}
			int nSecond = nBase + nDelta;
			Base[0][c] = (nBase << 3) | (nBase >> 2);
			Base[1][c] = ((nSecond & 0x1F) << 3) | ((nSecond & 0x1F) >> 2);
		}
		else
		{
			int nFirst = (nHigh >> (nShift + 4)) & 0xF;
			int nSecond = (nHigh >> nShift) & 0xF;
			Base[0][c] = nFirst * 17;
			Base[1][c] = nSecond * 17;
		}
	}

	for (unsigned x = 0; x < 4; x++)
	{
		for (unsigned y = 0; y < 4; y++)
		{
			unsigned nIndex = x * 4 + y;		// pixel indices run column-wise
			unsigned nSub = bFlip ? (y >= 2) : (x >= 2);
			unsigned nMSB = (nLow >> (nIndex + 16)) & 1;
			unsigned nLSB = (nLow >> nIndex) & 1;
			int nMod = Modifiers[nTable[nSub]][nMSB << 1 | nLSB];

			int r = Base[nSub][0] + nMod, g = Base[nSub][1] + nMod, b = Base[nSub][2] + nMod;
			r = r < 0 ? 0 : r > 255 ? 255 : r;
			g = g < 0 ? 0 : g > 255 ? 255 : g;
			b = b < 0 ? 0 : b > 255 ? 255 : b;

			Out[y * 4 + x] = RGBA (r, g, b, 255);	// row y (top row first), column x
		}
	}
}

//
// textures.cpp
//
// The tiled layouts follow Mesa's vc4 driver (vc4_resource.c: vc4_setup_slices,
// vc4_tiling.c: t_utile_address; MIT license), for 32 bits per pixel: a utile
// is 4x4 pixels (64 bytes), an LT image is rows of utiles, a T image is 4 KB
// tiles of 2x2 1 KB subtiles of 4x4 utiles.
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
	FORMAT_L8, FORMAT_A8, FORMAT_LA88, FORMAT_ETC1, FORMAT_RGB888, FORMATS
};

static const unsigned BytesPerPixel[FORMATS] = {4, 2, 2, 2, 1, 1, 2, 0, 3};

// TMU config (Mesa's kernel/vc4_packet.h)
#define TEX_TYPE_RGBA8888	0		// tiled
#define P0_TYPE__SHIFT		4
#define P0_CMMODE		(1 << 9)	// cube map
#define P1_HEIGHT__SHIFT	20
#define P1_WIDTH__SHIFT		8
#define P1_MAGFILT__SHIFT	7		// 0 linear, 1 nearest
#define P1_MINFILT__SHIFT	4
#define P1_WRAP_T__SHIFT	2
#define P1_WRAP_S__SHIFT	0		// 0 repeat, 1 clamp, 2 mirror
#define P2_PTYPE_CUBE_MAP_STRIDE (1U << 30)
#define P2_CMST__SHIFT		12		// cube map stride >> 12

#define RGBA(r, g, b, a)	((u32) (r) | (u32) (g) << 8 | (u32) (b) << 16 | (u32) (a) << 24)

static boolean IsPowerOfTwo (unsigned n)	{ return n && !(n & (n - 1)); }

static unsigned NextPowerOfTwo (unsigned n)
{
	unsigned p = 1;
	while (p < n)
	{
		p <<= 1;
	}
	return p;
}

static unsigned Align (unsigned n, unsigned a)	{ return (n + a - 1) & ~(a - 1); }

CTextures::CTextures (void)
:	m_nTotalBytes (0),
	m_nRetired (0),
	m_pFallback (nullptr)
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

// mip levels as Mesa's vc4_setup_slices, for 32 bits per pixel
void CTextures::Layout (TTexture *T)
{
	unsigned nPotWidth = NextPowerOfTwo (T->nWidth), nPotHeight = NextPowerOfTwo (T->nHeight);
	unsigned nOffset = 0;

	for (int i = T->nLevels - 1; i >= 0; i--)
	{
		TLevel &L = T->Levels[i];
		if (i == 0)
		{
			L.nWidth = T->nWidth;
			L.nHeight = T->nHeight;
		}
		else
		{
			L.nWidth = nPotWidth >> i ? nPotWidth >> i : 1;
			L.nHeight = nPotHeight >> i ? nPotHeight >> i : 1;
		}

		unsigned w, h;
		L.bT = !(L.nWidth <= 16 || L.nHeight <= 16);	// LT: up to 4 utiles in either direction
		if (L.bT)
		{
			w = Align (L.nWidth, 32);
			h = Align (L.nHeight, 32);
		}
		else
		{
			w = Align (L.nWidth, 4);
			h = Align (L.nHeight, 4);
		}
		L.nOffset = nOffset;
		L.nStride = w * 4;
		nOffset += h * L.nStride;
	}

	// level 0 must be page aligned: move all levels up
	unsigned nPad = Align (T->Levels[0].nOffset, 4096) - T->Levels[0].nOffset;
	for (unsigned i = 0; i < T->nLevels; i++)
	{
		T->Levels[i].nOffset += nPad;
	}
	T->nLevel0 = T->Levels[0].nOffset;

	unsigned nFaceBytes = nOffset + nPad;
	T->nFaceStride = Align (nFaceBytes, 4096);
	T->nBytes = T->bCube ? 6 * T->nFaceStride : nFaceBytes;
}

// the address of a pixel (vc4_tiling.c)
u32 *CTextures::TexelAddress (TTexture *T, unsigned nFace, unsigned nLevel, unsigned x, unsigned y)
{
	const TLevel &L = T->Levels[nLevel];
	unsigned ux = x >> 2, uy = y >> 2;		// utile
	unsigned nOffset;

	if (!L.bT)
	{
		nOffset = uy * 4 * L.nStride + ux * 64;
	}
	else
	{
		// t_utile_address: 4 KB tiles, odd tile rows right to left, and
		// the subtile order depends on the row
		unsigned nTileStride = L.nStride / 4 / 4 / 8;	// tiles per row
		unsigned tx = ux >> 3, ty = uy >> 3;
		boolean bOdd = ty & 1;
		if (bOdd)
		{
			tx = nTileStride - tx - 1;
		}
		static const u8 OddMap[4] = {2, 1, 3, 0};
		static const u8 EvenMap[4] = {0, 3, 1, 2};
		unsigned nSubtile = ((uy >> 2) & 1) << 1 | ((ux >> 2) & 1);
		nOffset =   4096 * (ty * nTileStride + tx)
			  + 1024 * (bOdd ? OddMap[nSubtile] : EvenMap[nSubtile])
			  + ((uy & 3) * 4 + (ux & 3)) * 64;	// utile in the subtile (LT)
	}
	nOffset += (y & 3) * 16 + (x & 3) * 4;			// pixel in the utile

	return (u32 *) (T->Storage.pBase + nFace * T->nFaceStride + L.nOffset + nOffset);
}

boolean CTextures::Alloc (TTexture *T, TStorage *pStorage)
{
	if (m_nTotalBytes + T->nBytes > MaxTotalBytes)
	{
		return FALSE;
	}

	pStorage->pRaw = new u8[T->nBytes + 4096];
	if (!pStorage->pRaw)
	{
		return FALSE;
	}
	pStorage->pBase = (u8 *) (((uintptr) pStorage->pRaw + 4095) & ~(uintptr) 4095);
	m_nTotalBytes += T->nBytes;

	return TRUE;
}

void CTextures::Free (TStorage *pStorage)
{
	delete [] pStorage->pRaw;
	pStorage->pRaw = nullptr;
	pStorage->pBase = nullptr;
}

// storage still referenced by the current frame: free it after the frame
void CTextures::Retire (TStorage *pStorage)
{
	if (m_nRetired < MaxRetired)
	{
		m_Retired[m_nRetired++] = *pStorage;
	}
	// else: too many replacements in one frame, leak rather than corrupt

	pStorage->pRaw = nullptr;
	pStorage->pBase = nullptr;
}

// a draw of this frame still samples the old contents: new storage
boolean CTextures::CopyOnWrite (TTexture *T)
{
	if (!T->bUsed)
	{
		return TRUE;
	}

	TStorage New;
	if (!Alloc (T, &New))
	{
		return FALSE;
	}
	memcpy (New.pBase, T->Storage.pBase, T->nBytes);
	m_nTotalBytes -= T->nBytes;		// the old storage is freed after the frame
	Retire (&T->Storage);
	T->Storage = New;
	T->bUsed = FALSE;

	return TRUE;
}

u32 CTextures::Create (u32 nId, unsigned nWidth, unsigned nHeight, u32 nFormat, boolean bCube)
{
	if (nId < 1 || nId > MaxTextures)
	{
		return PGPU_ERR_ID;
	}

	if (   nWidth < 1 || nWidth > MaxSize || nHeight < 1 || nHeight > MaxSize
	    || nFormat >= FORMATS || (bCube && nWidth != nHeight))
	{
		return PGPU_ERR_ENUM;
	}

	if (m_Textures[nId].bValid)
	{
		Delete (nId);
	}

	TTexture &T = m_Textures[nId];
	memset (&T, 0, sizeof T);
	T.nWidth = nWidth;
	T.nHeight = nHeight;
	T.nFormat = nFormat;
	T.bCube = bCube;
	T.nLevels = 1;
	if (IsPowerOfTwo (nWidth) && IsPowerOfTwo (nHeight))
	{
		for (unsigned n = nWidth > nHeight ? nWidth : nHeight; n > 1; n >>= 1)
		{
			T.nLevels++;
		}
	}
	T.nMinFilter = PGPU_NEAREST_MIPMAP_LINEAR;	// GL defaults
	T.nMagFilter = PGPU_LINEAR;
	Layout (&T);

	if (!Alloc (&T, &T.Storage))
	{
		return PGPU_ERR_MEMORY;
	}
	memset (T.Storage.pBase, 0, T.nBytes);
	CV3D::Flush (T.Storage.pBase, T.nBytes);
	T.bValid = TRUE;

	return 0;
}

u32 CTextures::Data (u32 nId, unsigned nLevel, unsigned nFace, unsigned x, unsigned y,
		     unsigned nWidth, unsigned nHeight, const u32 *pData, unsigned nWords)
{
	if (nId < 1 || nId > MaxTextures || !m_Textures[nId].bValid)
	{
		return PGPU_ERR_OBJECT;
	}

	TTexture &T = m_Textures[nId];
	if (nLevel >= T.nLevels || nFace >= (T.bCube ? 6U : 1U))
	{
		return PGPU_ERR_LIMIT;
	}
	const TLevel &L = T.Levels[nLevel];
	if (   x + nWidth > L.nWidth || y + nHeight > L.nHeight
	    || nWidth == 0 || nHeight == 0)
	{
		return PGPU_ERR_LIMIT;
	}

	if (!CopyOnWrite (&T))
	{
		return PGPU_ERR_MEMORY;
	}

	const u8 *pBytes = (const u8 *) pData;
	unsigned nBytes = nWords * 4;

	if (T.nFormat == FORMAT_ETC1)
	{
		// 4x4 blocks; blocks at the edge of a level may be partial
		if ((x | y) & 3)
		{
			return PGPU_ERR_LIMIT;
		}

		unsigned nBlocksX = (nWidth + 3) / 4, nBlocksY = (nHeight + 3) / 4;
		if (nBlocksX * nBlocksY * 8 > nBytes)
		{
			return PGPU_ERR_LENGTH;
		}

		for (unsigned by = 0; by < nBlocksY; by++)
		{
			for (unsigned bx = 0; bx < nBlocksX; bx++, pBytes += 8)
			{
				u32 Out[16];
				DecodeETC1 (pBytes, Out);
				// the block's first pixel row is the first row sent (t increasing)
				for (unsigned py = 0; py < 4 && by * 4 + py < nHeight; py++)
				{
					for (unsigned px = 0; px < 4 && bx * 4 + px < nWidth; px++)
					{
						*TexelAddress (&T, nFace, nLevel, x + bx * 4 + px, y + by * 4 + py)
							= Out[py * 4 + px];
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
			const u8 *s = pBytes + row * nRowBytes;		// row 0 = t 0

			for (unsigned i = 0; i < nWidth; i++)
			{
				u32 nPixel;
				switch (T.nFormat)
				{
				case FORMAT_RGBA8888:
					nPixel = RGBA (s[0], s[1], s[2], s[3]);
					s += 4;
					break;

				case FORMAT_RGB888:
					nPixel = RGBA (s[0], s[1], s[2], 255);
					s += 3;
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

				case FORMAT_A8:			// GL ES 2.0: (0, 0, 0, A)
					nPixel = RGBA (0, 0, 0, s[0]);
					s += 1;
					break;

				default:			// FORMAT_LA88
					nPixel = RGBA (s[0], s[0], s[0], s[1]);
					s += 2;
					break;
				}

				*TexelAddress (&T, nFace, nLevel, x + i, y + row) = nPixel;
			}
		}
	}

	CV3D::Flush (T.Storage.pBase, T.nBytes);

	// the level is defined (glTexImage2D; the Pico sends a level in parts)
	T.nDefined[nFace] |= 1 << nLevel;

	return 0;
}

// each level the average of 2x2 pixels of the level above (box filter)
u32 CTextures::GenerateMipmap (u32 nId)
{
	if (nId < 1 || nId > MaxTextures || !m_Textures[nId].bValid)
	{
		return PGPU_ERR_OBJECT;
	}

	TTexture &T = m_Textures[nId];
	if (T.nLevels == 1)
	{
		return 0;			// non-power-of-two: level 0 only
	}
	if (!CopyOnWrite (&T))
	{
		return PGPU_ERR_MEMORY;
	}

	unsigned nFaces = T.bCube ? 6 : 1;
	for (unsigned f = 0; f < nFaces; f++)
	{
		for (unsigned n = 1; n < T.nLevels; n++)
		{
			const TLevel &Src = T.Levels[n - 1], &Dst = T.Levels[n];
			for (unsigned y = 0; y < Dst.nHeight; y++)
			{
				for (unsigned x = 0; x < Dst.nWidth; x++)
				{
					unsigned x0 = 2 * x, y0 = 2 * y;
					unsigned x1 = x0 + 1 < Src.nWidth ? x0 + 1 : x0;
					unsigned y1 = y0 + 1 < Src.nHeight ? y0 + 1 : y0;
					u32 p[4] =
					{
						*TexelAddress (&T, f, n - 1, x0, y0), *TexelAddress (&T, f, n - 1, x1, y0),
						*TexelAddress (&T, f, n - 1, x0, y1), *TexelAddress (&T, f, n - 1, x1, y1)
					};
					u32 nPixel = 0;
					for (unsigned c = 0; c < 32; c += 8)
					{
						u32 s = 2;			// rounding
						for (unsigned k = 0; k < 4; k++)
						{
							s += (p[k] >> c) & 0xFF;
						}
						nPixel |= (s / 4) << c;
					}
					*TexelAddress (&T, f, n, x, y) = nPixel;
				}
			}
		}
		T.nDefined[f] = (1 << T.nLevels) - 1;
	}

	CV3D::Flush (T.Storage.pBase, T.nBytes);

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

	TTexture &T = m_Textures[nId];
	T.nMinFilter = nMinFilter;
	T.nMagFilter = nMagFilter;
	T.nWrapS = nWrapS;
	T.nWrapT = nWrapT;

	return 0;
}

u32 CTextures::Delete (u32 nId)
{
	if (nId < 1 || nId > MaxTextures || !m_Textures[nId].bValid)
	{
		return PGPU_ERR_OBJECT;
	}

	TTexture &T = m_Textures[nId];
	m_nTotalBytes -= T.nBytes;
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

// GL ES 2.0 3.8.2 (texture completeness): level 0 of every face defined, all
// levels with a mipmap filter; cube faces square (checked at creation)
boolean CTextures::Complete (const TTexture *T) const
{
	boolean bMipmap = T->nMinFilter >= PGPU_NEAREST_MIPMAP_NEAREST;
	u32 nNeeded = bMipmap ? (1 << T->nLevels) - 1 : 1;
	for (unsigned f = 0; f < (T->bCube ? 6U : 1U); f++)
	{
		if ((T->nDefined[f] & nNeeded) != nNeeded)
		{
			return FALSE;
		}
	}

	// non-power-of-two: CLAMP_TO_EDGE and no mipmaps (as GL ES 2.0)
	if (   T->nLevels == 1 && !(IsPowerOfTwo (T->nWidth) && IsPowerOfTwo (T->nHeight))
	    && (bMipmap || T->nWrapS != PGPU_CLAMP_TO_EDGE || T->nWrapT != PGPU_CLAMP_TO_EDGE))
	{
		return FALSE;
	}

	return TRUE;
}

boolean CTextures::Use (u32 nId, TConfig *pConfig)
{
	if (nId < 1 || nId > MaxTextures || !m_Textures[nId].bValid)
	{
		return FALSE;
	}

	TTexture &T = m_Textures[nId];
	if (!Complete (&T))
	{
		return FALSE;
	}
	T.bUsed = TRUE;

	// GL min filters (NEAREST, LINEAR, NEAREST_MIPMAP_NEAREST, LINEAR_MIPMAP_NEAREST,
	// NEAREST_MIPMAP_LINEAR, LINEAR_MIPMAP_LINEAR) -> TMU (0 linear, 1 nearest,
	// 2 near mip near, 3 near mip lin, 4 lin mip near, 5 lin mip lin)
	static const u8 MinFilter[6] = {1, 0, 2, 4, 3, 5};
	boolean bMipmap = T.nMinFilter >= PGPU_NEAREST_MIPMAP_NEAREST;

	pConfig->P0 =   CV3D::BusAddress (T.Storage.pBase + T.nLevel0)	// level 0, page aligned
		      | TEX_TYPE_RGBA8888 << P0_TYPE__SHIFT
		      | (bMipmap ? T.nLevels - 1 : 0)
		      | (T.bCube ? P0_CMMODE : 0);
	pConfig->P1 =   (T.nHeight & 2047) << P1_HEIGHT__SHIFT
		      | (T.nWidth & 2047) << P1_WIDTH__SHIFT
		      | (T.nMagFilter == PGPU_NEAREST ? 1 : 0) << P1_MAGFILT__SHIFT
		      | MinFilter[T.nMinFilter] << P1_MINFILT__SHIFT
		      | T.nWrapT << P1_WRAP_T__SHIFT
		      | T.nWrapS << P1_WRAP_S__SHIFT;
	pConfig->P2 = T.bCube ? P2_PTYPE_CUBE_MAP_STRIDE | (T.nFaceStride >> 12) << P2_CMST__SHIFT : 0;
	pConfig->bCube = T.bCube;
	pConfig->bAlphaFormat = T.nFormat == FORMAT_A8;

	return TRUE;
}

void CTextures::UseFallback (TConfig *pConfig)
{
	if (!m_pFallback)
	{
		// 1x1, LT: one utile of 4x4 pixels
		m_pFallback = (u32 *) CV3D::Alloc (64, 4096);
		for (unsigned i = 0; i < 16; i++)
		{
			m_pFallback[i] = RGBA (0, 0, 0, 255);
		}
		CV3D::Flush (m_pFallback, 64);
	}

	pConfig->P0 = CV3D::BusAddress (m_pFallback) | TEX_TYPE_RGBA8888 << P0_TYPE__SHIFT;
	pConfig->P1 =   1 << P1_HEIGHT__SHIFT | 1 << P1_WIDTH__SHIFT
		      | 1 << P1_MAGFILT__SHIFT | 1 << P1_MINFILT__SHIFT;
	pConfig->P2 = 0;
	pConfig->bCube = FALSE;
	pConfig->bAlphaFormat = FALSE;
}

boolean CTextures::GetRenderTarget (u32 nId, unsigned nFace, u32 *pBus, unsigned *pWidth,
				    unsigned *pHeight, boolean *pTFormat)
{
	if (   nId < 1 || nId > MaxTextures || !m_Textures[nId].bValid
	    || nFace >= (m_Textures[nId].bCube ? 6U : 1U))
	{
		return FALSE;
	}

	TTexture &T = m_Textures[nId];
	*pBus = CV3D::BusAddress (T.Storage.pBase + nFace * T.nFaceStride + T.Levels[0].nOffset);
	*pWidth = T.nWidth;
	*pHeight = T.nHeight;
	*pTFormat = T.Levels[0].bT;
	T.nDefined[nFace] |= 1;

	return TRUE;
}

void CTextures::Invalidate (u32 nId)
{
	if (nId >= 1 && nId <= MaxTextures && m_Textures[nId].bValid)
	{
		CV3D::Flush (m_Textures[nId].Storage.pBase, m_Textures[nId].nBytes);
	}
}

u32 CTextures::ReadRGBA (u32 nId, unsigned nFace, unsigned x, unsigned y)
{
	TTexture &T = m_Textures[nId];
	if (!T.bValid || x >= T.nWidth || y >= T.nHeight)
	{
		return 0;
	}

	return *TexelAddress (&T, nFace, 0, x, y);
}

u32 CTextures::WriteRGBA (u32 nId, unsigned nLevel, unsigned nFace, unsigned x, unsigned y,
			  unsigned nWidth, unsigned nHeight, const u32 *pPixels)
{
	if (nId < 1 || nId > MaxTextures || !m_Textures[nId].bValid)
	{
		return PGPU_ERR_OBJECT;
	}
	TTexture &T = m_Textures[nId];
	if (   nLevel >= T.nLevels || nFace >= (T.bCube ? 6U : 1U)
	    || x + nWidth > T.Levels[nLevel].nWidth || y + nHeight > T.Levels[nLevel].nHeight)
	{
		return PGPU_ERR_LIMIT;
	}
	if (!CopyOnWrite (&T))
	{
		return PGPU_ERR_MEMORY;
	}

	for (unsigned row = 0; row < nHeight; row++)
	{
		for (unsigned i = 0; i < nWidth; i++)
		{
			u32 nPixel = pPixels[row * nWidth + i];
			u32 r = nPixel & 0xFF, a = nPixel >> 24;
			switch (T.nFormat)		// as glCopyTexImage2D to this base format
			{
			case FORMAT_RGB565:
			case FORMAT_RGB888:
			case FORMAT_ETC1:	nPixel |= 0xFF000000;		break;
			case FORMAT_L8:		nPixel = RGBA (r, r, r, 255);	break;
			case FORMAT_A8:		nPixel = RGBA (0, 0, 0, a);	break;
			case FORMAT_LA88:	nPixel = RGBA (r, r, r, a);	break;
			default:						break;
			}
			*TexelAddress (&T, nFace, nLevel, x + i, y + row) = nPixel;
		}
	}
	CV3D::Flush (T.Storage.pBase, T.nBytes);
	T.nDefined[nFace] |= 1 << nLevel;

	return 0;
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

//
// v3dcheck.cpp
//
// Packet lengths and fields as in the VideoCore IV 3D Architecture Reference
// Guide and Mesa's vc4 (vc4_packet.h, vc4_validate.c); the lists this checks
// are gpu/renderer.cpp's (RenderJob, CopyToTiled).
//
#include "v3dcheck.h"
#include "v3d.h"
#include <circle/logger.h>
#include <circle/string.h>
#include <stdarg.h>

LOGMODULE ("v3dcheck");

// control list opcodes not in v3d.h
#define V3D_LINE_WIDTH		99
#define V3D_DEPTH_OFFSET	101

// the shader records (gpu/commands.cpp, gpu/renderer.cpp)
#define GL_RECORD_BYTES		36
#define GL_ATTRIBUTE_BYTES	8
#define NV_RECORD_BYTES		16

#define TILES_MAX		32		// 2048 pixels
#define TILE_STATE_BYTES	48		// V3D_TILE_STATE_SIZE

static unsigned s_nChecked, s_nRefused;

// ---- reading the lists ----------------------------------------------------------------

static const u8 *Ptr (u32 nBus)
{
	return (const u8 *) (uintptr) (nBus & ~0xC0000000U);	// BUS_ADDRESS () back (1:1 map)
}

static u16 Get16 (const u8 *p)	{ return p[0] | p[1] << 8; }
static u32 Get32 (const u8 *p)	{ return p[0] | p[1] << 8 | p[2] << 16 | (u32) p[3] << 24; }

static unsigned Align (unsigned n, unsigned a)	{ return (n + a - 1) / a * a; }

// bytes of a w x h image in memory (raster rows are w * bpp apart; T-format:
// 4 KB tiles of 32x32 or 64x32 pixels; LT: 64-byte utiles of 4x4 or 8x4)
static u32 ImageBytes (unsigned w, unsigned h, unsigned nBpp, unsigned nTiling)
{
	switch (nTiling)
	{
	case 0:		return w * h * nBpp;
	case 1:		return Align (w, nBpp == 4 ? 32 : 64) * Align (h, 32) * nBpp;
	default:	return Align (w, nBpp == 4 ? 4 : 8) * Align (h, 4) * nBpp;
	}
}

// ---- a failure ------------------------------------------------------------------------

static boolean Fail (const char *pList, u32 nStart, const u8 *p, const char *pFormat, ...)
{
	s_nRefused++;
	if (s_nRefused <= 10 || s_nRefused % 1000 == 0)
	{
		va_list Args;
		va_start (Args, pFormat);
		CString Why;
		Why.FormatV (pFormat, Args);
		va_end (Args);
		LOGERR ("Job refused (%u so far): %s list +%u, packet %u: %s", s_nRefused, pList,
			(unsigned) (p - Ptr (nStart)), (unsigned) *p, (const char *) Why);
	}

	return FALSE;
}

// ---- the binning list -----------------------------------------------------------------

struct TShaderState
{
	boolean bGL;			// else NV
	const u8 *pRecord;		// checked: inside a region
	unsigned nAttributes;		// GL
};

static boolean CheckShaderCode (u32 nCode, u32 nUniforms)
{
	return CV3D::InRegion (nCode, 8) && (!nUniforms || CV3D::InRegion (nUniforms, 4));
}

// the vertices nMin .. nMax of the current shader state
static boolean CheckVertices (const TShaderState &S, u32 nMin, u32 nMax)
{
	if (!S.bGL)
	{
		u32 nData = Get32 (S.pRecord + 12);
		unsigned nStride = S.pRecord[1];
		return CV3D::InRegion (nData + nMin * nStride, (nMax - nMin + 1) * nStride);
	}

	const u8 *pAttribute = S.pRecord + GL_RECORD_BYTES;
	for (unsigned i = 0; i < S.nAttributes; i++, pAttribute += GL_ATTRIBUTE_BYTES)
	{
		u32 nAddress = Get32 (pAttribute);
		unsigned nBytes = pAttribute[4] + 1;
		unsigned nStride = pAttribute[5];
		// (32-bit sums, as the V3D's: converted data starts at the smallest index)
		u32 nFirst = nAddress + nMin * nStride;
		if (!CV3D::InRegion (nFirst, (nMax - nMin) * nStride + nBytes))
		{
			return FALSE;
		}
	}

	return TRUE;
}

static boolean CheckBinning (u32 nStart, u32 nEnd)
{
	static const char List[] = "binning";
	const u8 *p = Ptr (nStart), *pEnd = Ptr (nEnd);
	boolean bConfig = FALSE, bStarted = FALSE, bFlushed = FALSE;
	TShaderState S = {FALSE, nullptr, 0};

	while (p < pEnd)
	{
		unsigned nLength;
		switch (*p)
		{
		case V3D_NOP:
		case V3D_FLUSH:
		case V3D_START_TILE_BINNING:		nLength = 1;	break;
		case V3D_CONFIGURATION_BITS:		nLength = 4;	break;
		case V3D_GL_SHADER_STATE:
		case V3D_NV_SHADER_STATE:
		case V3D_LINE_WIDTH:
		case V3D_DEPTH_OFFSET:
		case V3D_VIEWPORT_OFFSET:		nLength = 5;	break;
		case V3D_CLIP_WINDOW:
		case V3D_CLIPPER_XY_SCALING:
		case V3D_CLIPPER_Z_SCALE_OFFSET:	nLength = 9;	break;
		case V3D_VERTEX_ARRAY_PRIMITIVES:	nLength = 10;	break;
		case V3D_INDEXED_PRIMITIVE_LIST:	nLength = 14;	break;
		case V3D_TILE_BINNING_MODE_CONFIG:	nLength = 16;	break;
		default:
			return Fail (List, nStart, p, "not a packet this renderer emits");
		}
		if (p + nLength > pEnd)
		{
			return Fail (List, nStart, p, "cut off by the list's end");
		}
		if (bFlushed && *p != V3D_NOP)
		{
			return Fail (List, nStart, p, "after FLUSH");
		}
		if (!bStarted && *p != V3D_TILE_BINNING_MODE_CONFIG && *p != V3D_START_TILE_BINNING)
		{
			return Fail (List, nStart, p, "before START_TILE_BINNING");
		}

		switch (*p)
		{
		case V3D_TILE_BINNING_MODE_CONFIG: {
			u32 nAlloc = Get32 (p + 1), nAllocBytes = Get32 (p + 5), nState = Get32 (p + 9);
			unsigned nTilesX = p[13], nTilesY = p[14];
			if (bConfig)
			{
				return Fail (List, nStart, p, "a second binning mode");
			}
			if (!nTilesX || !nTilesY || nTilesX > TILES_MAX || nTilesY > TILES_MAX)
			{
				return Fail (List, nStart, p, "%u x %u tiles", nTilesX, nTilesY);
			}
			if (!CV3D::InRegion (nAlloc, nAllocBytes))
			{
				return Fail (List, nStart, p, "tile memory %08X, %u bytes: not the V3D's", nAlloc, nAllocBytes);
			}
			if (!CV3D::InRegion (nState, nTilesX * nTilesY * TILE_STATE_BYTES))
			{
				return Fail (List, nStart, p, "tile state %08X for %u tiles: not the V3D's",
					     nState, nTilesX * nTilesY);
			}
			bConfig = TRUE;
			} break;

		case V3D_START_TILE_BINNING:
			if (!bConfig || bStarted)
			{
				return Fail (List, nStart, p, bConfig ? "started twice" : "no binning mode before");
			}
			bStarted = TRUE;
			break;

		case V3D_FLUSH:
			bFlushed = TRUE;
			break;

		case V3D_GL_SHADER_STATE: {
			u32 nWord = Get32 (p + 1);
			u32 nRecord = nWord & ~15U;
			unsigned nAttributes = (nWord & 7) ? (nWord & 7) : 8;
			if (!CV3D::InRegion (nRecord, GL_RECORD_BYTES + nAttributes * GL_ATTRIBUTE_BYTES))
			{
				return Fail (List, nStart, p, "GL shader record %08X (%u attributes): not the V3D's",
					     nRecord, nAttributes);
			}
			const u8 *r = Ptr (nRecord);
			if (   !CheckShaderCode (Get32 (r + 4), Get32 (r + 8))
			    || !CheckShaderCode (Get32 (r + 16), Get32 (r + 20))
			    || !CheckShaderCode (Get32 (r + 28), Get32 (r + 32)))
			{
				return Fail (List, nStart, p, "GL shader record %08X: code or uniforms not the V3D's",
					     nRecord);
			}
			S.bGL = TRUE;
			S.pRecord = r;
			S.nAttributes = nAttributes;
			} break;

		case V3D_NV_SHADER_STATE: {
			u32 nRecord = Get32 (p + 1);
			if ((nRecord & 15) || !CV3D::InRegion (nRecord, NV_RECORD_BYTES))
			{
				return Fail (List, nStart, p, "NV shader record %08X: not the V3D's", nRecord);
			}
			const u8 *r = Ptr (nRecord);
			if (!CheckShaderCode (Get32 (r + 4), Get32 (r + 8)))
			{
				return Fail (List, nStart, p, "NV shader record %08X: code or uniforms not the V3D's",
					     nRecord);
			}
			S.bGL = FALSE;
			S.pRecord = r;
			S.nAttributes = 0;
			} break;

		case V3D_VERTEX_ARRAY_PRIMITIVES: {
			u32 nCount = Get32 (p + 2), nFirst = Get32 (p + 6);
			if (!S.pRecord)
			{
				return Fail (List, nStart, p, "a draw without shader state");
			}
			if (nCount && !CheckVertices (S, nFirst, nFirst + nCount - 1))
			{
				return Fail (List, nStart, p, "vertices %u .. %u: not the V3D's memory",
					     nFirst, nFirst + nCount - 1);
			}
			} break;

		case V3D_INDEXED_PRIMITIVE_LIST: {
			unsigned nIndexBytes = (p[1] >> 4) ? 2 : 1;
			u32 nCount = Get32 (p + 2), nIndices = Get32 (p + 6), nMaxIndex = Get32 (p + 10);
			if (!S.pRecord || !S.bGL)
			{
				return Fail (List, nStart, p, "an indexed draw without GL shader state");
			}
			if (!nCount)
			{
				break;
			}
			if (!CV3D::InRegion (nIndices, nCount * nIndexBytes))
			{
				return Fail (List, nStart, p, "%u indices at %08X: not the V3D's", nCount, nIndices);
			}
			// the indices it really uses (converted vertex data starts at the smallest)
			const u8 *pIndex = Ptr (nIndices);
			u32 nMin = 0xFFFFFFFF, nMax = 0;
			for (u32 i = 0; i < nCount; i++)
			{
				u32 n = nIndexBytes == 2 ? Get16 (pIndex + 2 * i) : pIndex[i];
				nMin = n < nMin ? n : nMin;
				nMax = n > nMax ? n : nMax;
			}
			if (nMax > nMaxIndex)
			{
				return Fail (List, nStart, p, "index %u over the max index %u", nMax, nMaxIndex);
			}
			if (!CheckVertices (S, nMin, nMax))
			{
				return Fail (List, nStart, p, "vertices %u .. %u: not the V3D's memory", nMin, nMax);
			}
			} break;

		default:
			break;
		}
		p += nLength;
	}

	if (!bFlushed)
	{
		return Fail (List, nStart, p, "no FLUSH at the end");
	}

	return TRUE;
}

// ---- the rendering list ---------------------------------------------------------------

static boolean CheckRendering (u32 nStart, u32 nEnd)
{
	static const char List[] = "rendering";
	const u8 *p = Ptr (nStart), *pEnd = Ptr (nEnd);
	boolean bMode = FALSE, bTile = FALSE, bEnded = FALSE;
	unsigned nWidth = 0, nHeight = 0, nTilesX = 0, nTilesY = 0;
	u16 LastFlags[2] = {0, 0};		// the last load and store that passed (every
	u32 LastWord[2] = {0, 0};		// tile repeats them)

	while (p < pEnd)
	{
		unsigned nLength;
		switch (*p)
		{
		case V3D_NOP:
		case V3D_STORE_MS_TILE_BUFFER:
		case V3D_STORE_MS_TILE_BUFFER_EOF:	nLength = 1;	break;
		case V3D_TILE_COORDINATES:		nLength = 3;	break;
		case V3D_BRANCH_TO_SUBLIST:		nLength = 5;	break;
		case V3D_STORE_TILE_BUFFER_GENERAL:
		case V3D_LOAD_TILE_BUFFER_GENERAL:	nLength = 7;	break;
		case V3D_TILE_RENDERING_MODE_CONFIG:	nLength = 11;	break;
		case V3D_CLEAR_COLORS:			nLength = 14;	break;
		default:
			return Fail (List, nStart, p, "not a packet this renderer emits");
		}
		if (p + nLength > pEnd)
		{
			return Fail (List, nStart, p, "cut off by the list's end");
		}
		if (bEnded && *p != V3D_NOP)
		{
			return Fail (List, nStart, p, "after the end-of-frame store");
		}
		if (!bMode && *p != V3D_TILE_RENDERING_MODE_CONFIG && *p != V3D_CLEAR_COLORS)
		{
			return Fail (List, nStart, p, "before the rendering mode");
		}

		switch (*p)
		{
		case V3D_TILE_RENDERING_MODE_CONFIG: {
			u32 nBuffer = Get32 (p + 1);
			nWidth = Get16 (p + 5);
			nHeight = Get16 (p + 7);
			u16 nFlags = Get16 (p + 9);
			unsigned nFormat = nFlags >> 2 & 3, nTiling = nFlags >> 6 & 3;
			if (bMode)
			{
				return Fail (List, nStart, p, "a second rendering mode");
			}
			if (   !nWidth || !nHeight || nWidth > TILES_MAX * V3D_TILE_SIZE
			    || nHeight > TILES_MAX * V3D_TILE_SIZE)
			{
				return Fail (List, nStart, p, "%u x %u pixels", nWidth, nHeight);
			}
			if ((nFlags & 3) || nFormat > 2 || nTiling > 2)
			{
				return Fail (List, nStart, p, "mode flags %04X (multisample, 64-bit colour, format "
					     "or tiling not this renderer's)", nFlags);
			}
			u32 nBytes = ImageBytes (nWidth, nHeight, nFormat == 1 ? 4 : 2, nTiling);
			if (!CV3D::InRegion (nBuffer, nBytes))
			{
				return Fail (List, nStart, p, "target %08X, %u x %u (%u bytes): not the V3D's",
					     nBuffer, nWidth, nHeight, nBytes);
			}
			nTilesX = (nWidth + V3D_TILE_SIZE-1) / V3D_TILE_SIZE;
			nTilesY = (nHeight + V3D_TILE_SIZE-1) / V3D_TILE_SIZE;
			bMode = TRUE;
			} break;

		case V3D_TILE_COORDINATES:
			if (p[1] >= nTilesX || p[2] >= nTilesY)
			{
				return Fail (List, nStart, p, "tile %u, %u outside %u x %u pixels",
					     p[1], p[2], nWidth, nHeight);
			}
			bTile = TRUE;
			break;

		case V3D_LOAD_TILE_BUFFER_GENERAL:
		case V3D_STORE_TILE_BUFFER_GENERAL: {
			u16 nFlags = Get16 (p + 1);
			u32 nWord = Get32 (p + 3);
			unsigned nBuffer = nFlags & 7, nTiling = nFlags >> 4 & 3, nFormat = nFlags >> 8 & 3;
			unsigned k = *p == V3D_STORE_TILE_BUFFER_GENERAL;
			if (nBuffer == 0 || (nFlags == LastFlags[k] && nWord == LastWord[k]))
			{
				break;			// none (a store that runs a load), or as before
			}
			if (nBuffer > 2 || nTiling > 2 || (nWord & 15))
			{
				return Fail (List, nStart, p, "buffer %u, tiling %u, address flags %X: not this "
					     "renderer's", nBuffer, nTiling, nWord & 15);
			}
			unsigned nBpp = nBuffer == 2 || nFormat == 0 ? 4 : 2;	// Z/stencil, RGBA8888; BGR565
			u32 nBytes = ImageBytes (nWidth, nHeight, nBpp, nTiling);
			if (!CV3D::InRegion (nWord, nBytes))
			{
				return Fail (List, nStart, p, "%s %s at %08X (%u bytes): not the V3D's",
					     *p == V3D_LOAD_TILE_BUFFER_GENERAL ? "load" : "store",
					     nBuffer == 2 ? "Z/stencil" : "colour", nWord, nBytes);
			}
			LastFlags[k] = nFlags;
			LastWord[k] = nWord;
			} break;

		case V3D_BRANCH_TO_SUBLIST: {
			u32 nSublist = Get32 (p + 1);
			if (!CV3D::InRegion (nSublist, V3D_TILE_ALLOC_BLOCK))
			{
				return Fail (List, nStart, p, "sublist %08X: not the V3D's", nSublist);
			}
			} break;

		case V3D_STORE_MS_TILE_BUFFER:
		case V3D_STORE_MS_TILE_BUFFER_EOF:
			if (!bTile)
			{
				return Fail (List, nStart, p, "a store before any tile coordinates");
			}
			bEnded = *p == V3D_STORE_MS_TILE_BUFFER_EOF;
			break;

		default:
			break;
		}
		p += nLength;
	}

	if (!bEnded)
	{
		return Fail (List, nStart, p, "no end-of-frame store");
	}

	return TRUE;
}

// ---- the job --------------------------------------------------------------------------

boolean V3DCheckJob (u32 nBinStart, u32 nBinEnd, u32 nRenderStart, u32 nRenderEnd)
{
	s_nChecked++;

	return    (!nBinStart || CheckBinning (nBinStart, nBinEnd))
	       && CheckRendering (nRenderStart, nRenderEnd);
}

void V3DCheckStats (unsigned *pChecked, unsigned *pRefused)
{
	*pChecked = s_nChecked;
	*pRefused = s_nRefused;
}

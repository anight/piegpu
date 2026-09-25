//
// commands.cpp
//
#include "commands.h"
#include <pgpu_protocol.h>
#include <circle/logger.h>
#include <v3d.h>
#include <circle/util.h>
#include <assert.h>

LOGMODULE ("commands");

#define VARIABLE		0xFFFF

// V3D configuration bits (first three bytes of the CONFIGURATION_BITS record)
#define CFG_FORWARD		(1 << 0)
#define CFG_REVERSE		(1 << 1)
#define CFG_CLOCKWISE		(1 << 2)	// clockwise primitives are forward facing
#define CFG_DEPTH_FUNC__SHIFT	12
#define CFG_Z_UPDATE		(1 << 15)

// TILE_RENDERING_MODE_CONFIG colour format (bits 3:2): 0 BGR565 dithered, 1 RGBA8888,
// 2 BGR565 without dither
#define MODE_BGR565_NO_DITHER	(2 << 2)

// GL shader record (VideoCore IV 3D reference guide; Mesa vc4 SHADER_RECORD)
#define SHADER_RECORD_BYTES	36
#define ATTRIBUTE_RECORD_BYTES	8
#define SR_FS_SINGLE_THREADED	(1 << 0)
#define SR_POINT_SIZE		(1 << 1)
#define SR_ENABLE_CLIPPING	(1 << 2)

#define MAX_PROGRAM_VERTICES	65535

// array component types (docs/protocol.md 10.5)
enum
{
	TYPE_FLOAT, TYPE_SHORT, TYPE_SHORT_NORM, TYPE_UBYTE_NORM, TYPE_BYTE_NORM,
	TYPE_UBYTE, TYPE_BYTE, TYPE_USHORT, TYPE_USHORT_NORM, TYPE_FIXED, TYPES
};
static const unsigned TypeBytes[TYPES] = {4, 2, 2, 1, 1, 1, 1, 2, 2, 4};

static float AsFloat (u32 nWord)
{
	float f;
	memcpy (&f, &nWord, sizeof f);
	return f;
}

static void Identity (float *m)
{
	memset (m, 0, 16 * sizeof (float));
	m[0] = m[5] = m[10] = m[15] = 1.0f;
}

static void SetColor (float *pColor, u32 nColor)
{
	pColor[0] = (nColor & 0xFF) / 255.0f;
	pColor[1] = ((nColor >> 8) & 0xFF) / 255.0f;
	pColor[2] = ((nColor >> 16) & 0xFF) / 255.0f;
	pColor[3] = (nColor >> 24) / 255.0f;
}

static void Set4 (float *p, float a, float b, float c, float d)
{
	p[0] = a; p[1] = b; p[2] = c; p[3] = d;
}

static float Component (const u8 *p, u32 nType)
{
	switch (nType)
	{
	case TYPE_FLOAT: {
		float f;
		memcpy (&f, p, sizeof f);
		return f;
		}

	case TYPE_SHORT:
	case TYPE_SHORT_NORM: {
		s16 s = (s16) (p[0] | p[1] << 8);
		if (nType == TYPE_SHORT)
		{
			return s;
		}
		float f = s / 32767.0f;
		return f < -1.0f ? -1.0f : f;
		}

	case TYPE_UBYTE_NORM:
		return p[0] / 255.0f;

	case TYPE_BYTE_NORM: {
		float f = (s8) p[0] / 127.0f;
		return f < -1.0f ? -1.0f : f;
		}

	case TYPE_UBYTE:
		return p[0];

	case TYPE_BYTE:
		return (s8) p[0];

	case TYPE_USHORT:
		return (u16) (p[0] | p[1] << 8);

	case TYPE_FIXED:			// 16.16
		return (s32) (p[0] | p[1] << 8 | p[2] << 16 | (u32) p[3] << 24) / 65536.0f;

	default:
		return (u16) (p[0] | p[1] << 8) / 65535.0f;
	}
}

// a generic attribute's current value in an array format (little endian)
static void ConvertValue (const float *pValue, u32 nType, unsigned nSize, u8 *pOut)
{
	for (unsigned c = 0; c < nSize; c++)
	{
		float f = pValue[c];
		float n = f < -1.0f ? -1.0f : f > 1.0f ? 1.0f : f;
		float u = f < 0.0f ? 0.0f : f > 1.0f ? 1.0f : f;
		s32 v;
		switch (nType)
		{
		case TYPE_FLOAT:	memcpy (pOut + 4 * c, &f, 4);	continue;
		case TYPE_SHORT:	v = (s32) f;			break;
		case TYPE_SHORT_NORM:	v = (s32) (n * 32767.0f);	break;
		case TYPE_UBYTE_NORM:	v = (s32) (u * 255.0f + 0.5f);	break;
		case TYPE_BYTE_NORM:	v = (s32) (n * 127.0f);		break;
		case TYPE_UBYTE:
		case TYPE_BYTE:
		case TYPE_USHORT:	v = (s32) f;			break;
		case TYPE_FIXED:	v = (s32) (f * 65536.0f);	break;
		default:		v = (s32) (u * 65535.0f + 0.5f); break;
		}

		unsigned nBytes = TypeBytes[nType];
		for (unsigned b = 0; b < nBytes; b++)
		{
			pOut[nBytes * c + b] = (u8) (v >> (8 * b));
		}
	}
}

// Convert nCount elements of an array into another format (missing
// components (0, 0, 0, 1), as in GL) into the frame's vertex pool
static u8 *ConvertArray (CRenderer *pRenderer, const u8 *pSrc, unsigned nSrcStride, u32 nSrcType,
			 unsigned nSrcSize, unsigned nCount, u32 nDstType, unsigned nDstSize, u32 *pBus)
{
	unsigned nDstBytes = nDstSize * TypeBytes[nDstType];
	u8 *pDst = pRenderer->AllocData (nCount * nDstBytes, pBus);
	if (!pDst)
	{
		return nullptr;
	}

	u8 *p = pDst;
	for (unsigned i = 0; i < nCount; i++, pSrc += nSrcStride, p += nDstBytes)
	{
		float v[4] = {0.0f, 0.0f, 0.0f, 1.0f};
		for (unsigned k = 0; k < nSrcSize; k++)
		{
			v[k] = Component (pSrc + k * TypeBytes[nSrcType], nSrcType);
		}
		ConvertValue (v, nDstType, nDstSize, p);
	}

	return pDst;
}

CCommands::CCommands (CRenderer *pRenderer, CReceiver *pReceiver)
:	m_pRenderer (pRenderer),
	m_pReceiver (pReceiver),
	m_pHostLink (nullptr),
	m_Geometry (pRenderer, &m_Textures),
	m_nBufferBytes (0),
	m_nRetiredBuffers (0),
	m_nProgram (0),
	m_nFramebuffer (0),
	m_nFrameNumber (0),
	m_nLastFrameUs (0),
	m_nTotalFrames (0),
	m_nTotalErrors (0)
{
	memset (m_Buffers, 0, sizeof m_Buffers);
	m_pInput = new TInputVertex[CGeometry::MaxVertices];
	m_pIndices = new u32[CGeometry::MaxVertices];
	memset (&m_Stats, 0, sizeof m_Stats);

	memset (m_Framebuffers, 0, sizeof m_Framebuffers);
	memset (m_SharedZS, 0, sizeof m_SharedZS);
	memset (&m_FrameStats, 0, sizeof m_FrameStats);
	memset (&m_Load, 0, sizeof m_Load);
	m_bJobPending = m_bJobClearColor = m_bJobClearZS = FALSE;
	m_bPanelDrawn = m_bPanelZSValid = FALSE;
	m_Geometry.SetJobFullHandler (JobFullHandler, this);

	DefaultState ();
}

CCommands::~CCommands (void)
{
}

void CCommands::Reset (void)
{
	for (unsigned i = 1; i <= MaxBuffers; i++)
	{
		BufferDelete (i);
	}
	m_Textures.Reset ();
	m_Programs.Reset ();
	m_pRenderer->DiscardFrame ();
	m_Textures.EndFrame ();
	m_Programs.EndFrame ();
	for (unsigned i = 0; i < m_nRetiredBuffers; i++)
	{
		delete [] m_RetiredBuffers[i];
	}
	m_nRetiredBuffers = 0;

	for (unsigned i = 1; i <= MaxFramebuffers; i++)
	{
		FramebufferFree (&m_Framebuffers[i]);
	}
	for (unsigned i = 1; i <= MaxSharedZS; i++)
	{
		delete [] m_SharedZS[i].p;
		memset (&m_SharedZS[i], 0, sizeof m_SharedZS[i]);
	}
	m_nFramebuffer = 0;
	m_bJobPending = m_bJobClearColor = m_bJobClearZS = FALSE;
	m_bPanelDrawn = FALSE;

	DefaultState ();
}

// docs/protocol.md 7.9
void CCommands::DefaultState (void)
{
	TGLState &S = m_State;
	memset (&S, 0, sizeof S);
	S.nEnables = PGPU_CAP_DITHER;		// as GL

	Identity (S.Modelview);
	Identity (S.Projection);
	Identity (S.Texture);

	S.ViewportW = m_pRenderer->GetWidth ();
	S.ViewportH = m_pRenderer->GetHeight ();
	UpdateTarget ();
	S.DepthFar = 1.0f;

	S.nDepthFunc = PGPU_LESS;
	S.bDepthMask = TRUE;
	S.nBlendSrc = S.nBlendSrcA = PGPU_ONE;
	S.nBlendDst = S.nBlendDstA = PGPU_ZERO;
	S.nBlendEqRGB = S.nBlendEqA = PGPU_FUNC_ADD;
	for (unsigned f = 0; f < 2; f++)
	{
		S.StencilFunc[f] = PGPU_ALWAYS;
		S.StencilValueMask[f] = S.StencilWriteMask[f] = 0xFF;
		S.StencilFail[f] = S.StencilZFail[f] = S.StencilZPass[f] = PGPU_KEEP;
	}
	S.nCullFace = PGPU_BACK;
	S.nFrontFace = PGPU_CCW;
	S.nAlphaFunc = PGPU_ALWAYS;
	S.nColorMask = 0xF;
	S.ScissorW = m_pRenderer->GetWidth ();
	S.ScissorH = m_pRenderer->GetHeight ();
	S.fLineWidth = 1.0f;

	Set4 (S.Color, 1, 1, 1, 1);
	S.Normal[2] = 1.0f;

	for (unsigned i = 0; i < 4; i++)
	{
		TLight &L = S.Lights[i];
		Set4 (L.Position, 0, 0, 1, 0);
		Set4 (L.Ambient, 0, 0, 0, 1);
		float f = i == 0 ? 1.0f : 0.0f;
		Set4 (L.Diffuse, f, f, f, 1);
		Set4 (L.Specular, f, f, f, 1);
		L.Attenuation[0] = 1.0f;
	}
	Set4 (S.MatAmbient, 0.2f, 0.2f, 0.2f, 1);
	Set4 (S.MatDiffuse, 0.8f, 0.8f, 0.8f, 1);
	Set4 (S.MatSpecular, 0, 0, 0, 1);
	Set4 (S.MatEmission, 0, 0, 0, 1);
	Set4 (S.LightModelAmbient, 0.2f, 0.2f, 0.2f, 1);

	S.nFogMode = 1;			// EXP
	S.fFogEnd = 1.0f;
	S.fFogDensity = 1.0f;

	memset (m_Arrays, 0, sizeof m_Arrays);
	m_nArraysEnabled = 0;

	m_nProgram = 0;
	memset (m_Generic, 0, sizeof m_Generic);
	m_nGenericEnabled = 0;
	for (unsigned i = 0; i < PGPU_MAX_ATTRIBUTES; i++)
	{
		Set4 (m_GenericValue[i], 0, 0, 0, 1);
	}
	memset (m_TextureUnit, 0, sizeof m_TextureUnit);
}

void CCommands::SendInfo (void)
{
	u32 Info[7] =
	{
		PGPU_VERSION,
		m_pRenderer->GetWidth () | m_pRenderer->GetHeight () << 16,
		CTextures::MaxSize,
		MaxBuffers,
		CTextures::MaxTextures,
		4,
		CReceiver::RingWords * 4
	};
	Reply (PGPU_REPLY_INFO, Info, 7);
}

void CCommands::Execute (u32 nHeader, const u32 *pPayload)
{
	u32 nOpcode = PGPU_HEADER_OP (nHeader);
	unsigned nLength = PGPU_HEADER_LEN (nHeader);

	m_Stats.nCommands++;

	// payload length of each command, VARIABLE = checked by the handler
	static const struct { u8 uchOpcode; u16 usLength; } Lengths[] =
	{
		{PGPU_OP_RESET, 0}, {PGPU_OP_GET_INFO, 0}, {PGPU_OP_PING, 1}, {PGPU_OP_GET_STATUS, 0},
		{PGPU_OP_CLEAR, VARIABLE}, {PGPU_OP_FRAME_END, 1}, {PGPU_OP_VIEWPORT, 6},
		{PGPU_OP_FRAMEBUFFER_CREATE, 3}, {PGPU_OP_FRAMEBUFFER_DELETE, 1},
		{PGPU_OP_BIND_FRAMEBUFFER, 1}, {PGPU_OP_READ_PIXELS, 4}, {PGPU_OP_COPY_TEX_IMAGE, 6},
		{PGPU_OP_STENCIL_FUNC, 4}, {PGPU_OP_STENCIL_OP, 4}, {PGPU_OP_STENCIL_MASK, 2},
		{PGPU_OP_BUFFER_CREATE, 2}, {PGPU_OP_BUFFER_DATA, VARIABLE}, {PGPU_OP_BUFFER_DELETE, 1},
		{PGPU_OP_TEXTURE_CREATE, 3}, {PGPU_OP_TEXTURE_DATA, VARIABLE}, {PGPU_OP_TEXTURE_PARAMS, 5},
		{PGPU_OP_TEXTURE_DELETE, 1}, {PGPU_OP_TEXTURE_BIND, 1}, {PGPU_OP_TEX_ENV, 2},
		{PGPU_OP_GENERATE_MIPMAP, 1},
		{PGPU_OP_ENABLE, 1}, {PGPU_OP_DISABLE, 1}, {PGPU_OP_DEPTH_FUNC, 1},
		{PGPU_OP_DEPTH_MASK, 1}, {PGPU_OP_BLEND_FUNC, 2}, {PGPU_OP_CULL_FACE, 1},
		{PGPU_OP_FRONT_FACE, 1}, {PGPU_OP_ALPHA_FUNC, 2}, {PGPU_OP_COLOR_MASK, 1},
		{PGPU_OP_SCISSOR, 4}, {PGPU_OP_POLYGON_OFFSET, 2}, {PGPU_OP_LINE_WIDTH, 1},
		{PGPU_OP_BLEND_FUNC_SEPARATE, 4}, {PGPU_OP_BLEND_EQUATION, 2}, {PGPU_OP_BLEND_COLOR, 4},
		{PGPU_OP_LOAD_MATRIX, 17}, {PGPU_OP_LIGHT, 11}, {PGPU_OP_MATERIAL, 5},
		{PGPU_OP_LIGHT_MODEL, 2}, {PGPU_OP_FOG, 5}, {PGPU_OP_SHADE_MODEL, 1},
		{PGPU_OP_COLOR, 1}, {PGPU_OP_NORMAL, 3}, {PGPU_OP_TEXCOORD, 2},
		{PGPU_OP_ARRAY, 6}, {PGPU_OP_ARRAYS_ENABLE, 1}, {PGPU_OP_DRAW_ARRAYS, 3},
		{PGPU_OP_DRAW_ELEMENTS, 5}, {PGPU_OP_DRAW_INLINE, VARIABLE},
		{PGPU_OP_PROGRAM_CREATE, 2}, {PGPU_OP_PROGRAM_DATA, VARIABLE},
		{PGPU_OP_PROGRAM_DELETE, 1}, {PGPU_OP_USE_PROGRAM, 1},
		{PGPU_OP_PROGRAM_UNIFORM, VARIABLE}, {PGPU_OP_PROGRAM_SAMPLER, 3},
		{PGPU_OP_TEXTURE_BIND_UNIT, 2}, {PGPU_OP_VERTEX_ATTRIB, 5},
		{PGPU_OP_ATTRIB_ARRAY, 6}, {PGPU_OP_ATTRIBS_ENABLE, 1},
		{PGPU_OP_PROGRAM_DRAW_INLINE, VARIABLE},
	};

	boolean bKnown = FALSE;
	for (auto &L : Lengths)
	{
		if (L.uchOpcode == nOpcode)
		{
			bKnown = TRUE;
			if (L.usLength != VARIABLE && L.usLength != nLength)
			{
				Error (PGPU_ERR_LENGTH, nOpcode, nLength);
				return;
			}
			break;
		}
	}
	if (!bKnown)
	{
		Error (PGPU_ERR_OPCODE, nOpcode, 0);
		return;
	}

	u32 nDetail = 0;
	u32 nError = Dispatch (nOpcode, pPayload, nLength, &nDetail);
	if (   nError == PGPU_ERR_MEMORY && m_pRenderer->GetDraws ()
	    && (   nOpcode == PGPU_OP_DRAW_ARRAYS || nOpcode == PGPU_OP_DRAW_ELEMENTS
		|| nOpcode == PGPU_OP_PROGRAM_DRAW_INLINE || nOpcode == PGPU_OP_CLEAR))
	{
		// the frame's pools are full: render the job so far, draw into the next
		FlushJob (FALSE);
		nDetail = 0;
		nError = Dispatch (nOpcode, pPayload, nLength, &nDetail);
	}
	if (nError)
	{
		if (nError == PGPU_ERR_LENGTH)
		{
			nDetail = nLength;
		}
		Error (nError, nOpcode, nDetail);
	}
}

u32 CCommands::Dispatch (u32 nOpcode, const u32 *p, unsigned nLength, u32 *pDetail)
{
	TGLState &S = m_State;

	switch (nOpcode)
	{
	case PGPU_OP_RESET:
		Reset ();
		LOGNOTE ("RESET");
		SendInfo ();
		break;

	case PGPU_OP_GET_INFO:
		SendInfo ();
		break;

	case PGPU_OP_PING:
		Reply (PGPU_REPLY_PONG, p, 1);
		break;

	case PGPU_OP_GET_STATUS: {
		u32 Status[10] =
		{
			m_nTotalFrames,
			m_pReceiver->GetTotalCRCErrors (),
			m_nTotalErrors,
			m_pReceiver->GetFreeBytes (),
			m_nLastFrameUs,
			m_Load.nWindowUs,
			m_Load.nFrames,
			m_Load.nV3DBusyUs,
			m_Load.nARMBusyUs,
			m_Load.nPanelWaitUs
		};
		Reply (PGPU_REPLY_STATUS, Status, 10);
		} break;

	// frame

	case PGPU_OP_CLEAR:
		if (nLength != 3 && nLength != 4)		// mask, colour, depth [, stencil]
		{
			return PGPU_ERR_LENGTH;
		}
		if (p[0] & ~(PGPU_CLEAR_COLOR | PGPU_CLEAR_DEPTH | PGPU_CLEAR_STENCIL))
		{
			return PGPU_ERR_ENUM;
		}
		return Clear (p[0], p[1], AsFloat (p[2]), nLength == 4 ? p[3] & 0xFF : 0);

	case PGPU_OP_FRAMEBUFFER_CREATE:
		*pDetail = p[0];
		return FramebufferCreate (p[0], p[1], p[2]);

	case PGPU_OP_FRAMEBUFFER_DELETE:
		*pDetail = p[0];
		if (p[0] < 1 || p[0] > MaxFramebuffers)
		{
			return PGPU_ERR_ID;
		}
		if (!m_Framebuffers[p[0]].bValid)
		{
			return PGPU_ERR_OBJECT;
		}
		if (p[0] == m_nFramebuffer)
		{
			FlushJob (FALSE);
			m_nFramebuffer = 0;
			UpdateTarget ();
		}
		FramebufferFree (&m_Framebuffers[p[0]]);
		break;

	case PGPU_OP_BIND_FRAMEBUFFER:
		*pDetail = p[0];
		if (p[0] > MaxFramebuffers)
		{
			return PGPU_ERR_ID;
		}
		if (p[0] && !m_Framebuffers[p[0]].bValid)
		{
			return PGPU_ERR_OBJECT;
		}
		if (p[0] != m_nFramebuffer)
		{
			FlushJob (FALSE);		// render what the old target collected
			m_nFramebuffer = p[0];
		}
		UpdateTarget ();
		break;

	case PGPU_OP_READ_PIXELS: {
		// x, y, width, height -> PIXELS replies (word offset, RGBA8888 rows bottom up)
		unsigned nWords = p[2] * p[3];
		if (nWords == 0 || nWords > 256 * 1024)
		{
			return PGPU_ERR_LIMIT;
		}
		u32 *pPixels = new u32[nWords];
		u32 nError = ReadRect ((s32) p[0], (s32) p[1], p[2], p[3], pPixels);
		unsigned nRetries = 0;
		for (unsigned nOffset = 0; !nError && nOffset < nWords && nRetries < 1000000; )
		{
			u32 Reply[62];
			unsigned n = nWords - nOffset < 61 ? nWords - nOffset : 61;
			Reply[0] = nOffset;
			memcpy (Reply + 1, pPixels + nOffset, n * 4);
			if (!this->Reply (PGPU_REPLY_PIXELS, Reply, 1 + n))
			{
				m_pReceiver->UpdateTx ();		// backlog full: wait for the link
				nRetries++;
				continue;
			}
			nOffset += n;
		}
		delete [] pPixels;
		return nError;
		}

	case PGPU_OP_COPY_TEX_IMAGE: {
		// texture | level << 16 | face << 24, xoffset | yoffset << 16, x, y, width, height
		unsigned nWords = p[4] * p[5];
		if (nWords == 0 || nWords > 256 * 1024)
		{
			return PGPU_ERR_LIMIT;
		}
		*pDetail = p[0] & 0xFFFF;
		u32 *pPixels = new u32[nWords];
		u32 nError = ReadRect ((s32) p[2], (s32) p[3], p[4], p[5], pPixels);
		if (!nError)
		{
			nError = m_Textures.WriteRGBA (p[0] & 0xFFFF, (p[0] >> 16) & 0xFF, p[0] >> 24,
						       p[1] & 0xFFFF, p[1] >> 16, p[4], p[5], pPixels);
		}
		delete [] pPixels;
		return nError;
		}

	case PGPU_OP_FRAME_END:
		EndFrame (p[0]);
		break;

	case PGPU_OP_VIEWPORT:
		S.ViewportX = (float) (s32) p[0];
		S.ViewportY = (float) (s32) p[1];
		S.ViewportW = (float) p[2];
		S.ViewportH = (float) p[3];
		S.DepthNear = AsFloat (p[4]);
		S.DepthFar = AsFloat (p[5]);
		break;

	// buffers

	case PGPU_OP_BUFFER_CREATE:
		*pDetail = p[0];
		return BufferCreate (p[0], p[1]);

	case PGPU_OP_BUFFER_DATA:
		*pDetail = nLength >= 1 ? p[0] : 0;
		return BufferData (p, nLength);

	case PGPU_OP_BUFFER_DELETE:
		*pDetail = p[0];
		if (p[0] < 1 || p[0] > MaxBuffers)
		{
			return PGPU_ERR_ID;
		}
		return BufferDelete (p[0]);

	// textures

	case PGPU_OP_TEXTURE_CREATE:
		*pDetail = p[0];
		if (p[2] & ~(0xFFu | PGPU_TEXTURE_CUBE))
		{
			return PGPU_ERR_ENUM;
		}
		return m_Textures.Create (p[0], p[1] & 0xFFFF, p[1] >> 16, p[2] & 0xFF,
					  !!(p[2] & PGPU_TEXTURE_CUBE));

	case PGPU_OP_TEXTURE_DATA:
		if (nLength < 3)
		{
			return PGPU_ERR_LENGTH;
		}
		*pDetail = p[0];
		FlushIfTarget (p[0] & 0xFFFF);
		return m_Textures.Data (p[0] & 0xFFFF, (p[0] >> 16) & 0xFF, p[0] >> 24,
					p[1] & 0xFFFF, p[1] >> 16, p[2] & 0xFFFF, p[2] >> 16,
					p + 3, nLength - 3);

	case PGPU_OP_GENERATE_MIPMAP:
		*pDetail = p[0];
		FlushIfTarget (p[0]);
		return m_Textures.GenerateMipmap (p[0]);

	case PGPU_OP_TEXTURE_PARAMS:
		*pDetail = p[0];
		return m_Textures.Params (p[0], p[1], p[2], p[3], p[4]);

	case PGPU_OP_TEXTURE_DELETE:
		*pDetail = p[0];
		if (p[0] < 1 || p[0] > CTextures::MaxTextures)
		{
			return PGPU_ERR_ID;
		}
		return m_Textures.Delete (p[0]);

	case PGPU_OP_TEXTURE_BIND:
		if (p[0] > CTextures::MaxTextures)
		{
			*pDetail = p[0];
			return PGPU_ERR_ID;
		}
		S.nBoundTexture = p[0];
		m_TextureUnit[0] = p[0];
		break;

	case PGPU_OP_TEXTURE_BIND_UNIT:
		if (p[0] >= PGPU_MAX_TEXTURE_UNITS)
		{
			return PGPU_ERR_LIMIT;
		}
		if (p[1] > CTextures::MaxTextures)
		{
			*pDetail = p[1];
			return PGPU_ERR_ID;
		}
		m_TextureUnit[p[0]] = p[1];
		if (p[0] == 0)
		{
			S.nBoundTexture = p[1];
		}
		break;

	case PGPU_OP_TEX_ENV:
		if (p[0] > 3)
		{
			return PGPU_ERR_ENUM;
		}
		S.nTexEnvMode = p[0];
		SetColor (S.TexEnvColor, p[1]);
		break;

	// fragment state

	case PGPU_OP_ENABLE:
	case PGPU_OP_DISABLE:
		if (p[0] & ~PGPU_CAP_ALL)
		{
			return PGPU_ERR_ENUM;
		}
		if (nOpcode == PGPU_OP_ENABLE)
		{
			S.nEnables |= p[0];
		}
		else
		{
			S.nEnables &= ~p[0];
		}
		break;

	case PGPU_OP_DEPTH_FUNC:
		if (p[0] > PGPU_ALWAYS)
		{
			return PGPU_ERR_ENUM;
		}
		S.nDepthFunc = p[0];
		break;

	case PGPU_OP_DEPTH_MASK:
		S.bDepthMask = p[0] != 0;
		break;

	case PGPU_OP_BLEND_FUNC:
	case PGPU_OP_BLEND_FUNC_SEPARATE: {
		// SRC_ALPHA_SATURATE is a source factor only
		boolean bSeparate = nOpcode == PGPU_OP_BLEND_FUNC_SEPARATE;
		u32 nSrcA = bSeparate ? p[2] : p[0], nDstA = bSeparate ? p[3] : p[1];
		if (   p[0] > PGPU_ONE_MINUS_CONSTANT_ALPHA || nSrcA > PGPU_ONE_MINUS_CONSTANT_ALPHA
		    || p[1] > PGPU_ONE_MINUS_CONSTANT_ALPHA || nDstA > PGPU_ONE_MINUS_CONSTANT_ALPHA
		    || p[1] == PGPU_SRC_ALPHA_SATURATE || nDstA == PGPU_SRC_ALPHA_SATURATE)
		{
			return PGPU_ERR_ENUM;
		}
		S.nBlendSrc = p[0];
		S.nBlendDst = p[1];
		S.nBlendSrcA = nSrcA;
		S.nBlendDstA = nDstA;
		} break;

	case PGPU_OP_BLEND_EQUATION:
		if (p[0] > PGPU_FUNC_REVERSE_SUBTRACT || p[1] > PGPU_FUNC_REVERSE_SUBTRACT)
		{
			return PGPU_ERR_ENUM;
		}
		S.nBlendEqRGB = p[0];
		S.nBlendEqA = p[1];
		break;

	case PGPU_OP_BLEND_COLOR:
		for (unsigned i = 0; i < 4; i++)
		{
			float f = AsFloat (p[i]);
			S.BlendColor[i] = f < 0.0f ? 0.0f : f > 1.0f ? 1.0f : f;	// clamped (ES 2.0)
		}
		break;

	case PGPU_OP_CULL_FACE:
		if (p[0] > PGPU_FRONT_AND_BACK)
		{
			return PGPU_ERR_ENUM;
		}
		S.nCullFace = p[0];
		break;

	case PGPU_OP_FRONT_FACE:
		if (p[0] > PGPU_CW)
		{
			return PGPU_ERR_ENUM;
		}
		S.nFrontFace = p[0];
		break;

	case PGPU_OP_ALPHA_FUNC:
		if (p[0] > PGPU_ALWAYS)
		{
			return PGPU_ERR_ENUM;
		}
		S.nAlphaFunc = p[0];
		S.fAlphaRef = AsFloat (p[1]);
		break;

	case PGPU_OP_COLOR_MASK:
		if (p[0] & ~0xFu)
		{
			return PGPU_ERR_ENUM;
		}
		S.nColorMask = p[0];
		break;

	case PGPU_OP_STENCIL_FUNC:
	case PGPU_OP_STENCIL_OP:
		if (   p[0] > PGPU_FRONT_AND_BACK || p[1] > 7
		    || (nOpcode == PGPU_OP_STENCIL_OP && (p[2] > 7 || p[3] > 7)))
		{
			return PGPU_ERR_ENUM;
		}
		for (unsigned f = 0; f < 2; f++)
		{
			if (p[0] != PGPU_FRONT_AND_BACK && p[0] != f)
			{
				continue;
			}
			if (nOpcode == PGPU_OP_STENCIL_FUNC)
			{
				S.StencilFunc[f] = p[1];
				S.StencilRef[f] = p[2] > 255 ? 255 : p[2];	// clamped to the 8 bits
				S.StencilValueMask[f] = p[3] & 0xFF;
			}
			else
			{
				S.StencilFail[f] = p[1];
				S.StencilZFail[f] = p[2];
				S.StencilZPass[f] = p[3];
			}
		}
		break;

	case PGPU_OP_STENCIL_MASK:
		if (p[0] > PGPU_FRONT_AND_BACK)
		{
			return PGPU_ERR_ENUM;
		}
		for (unsigned f = 0; f < 2; f++)
		{
			if (p[0] == PGPU_FRONT_AND_BACK || p[0] == f)
			{
				S.StencilWriteMask[f] = p[1] & 0xFF;
			}
		}
		break;

	case PGPU_OP_SCISSOR:
		S.ScissorX = (s32) p[0];
		S.ScissorY = (s32) p[1];
		S.ScissorW = p[2];
		S.ScissorH = p[3];
		break;

	case PGPU_OP_POLYGON_OFFSET:
		S.fOffsetFactor = AsFloat (p[0]);
		S.fOffsetUnits = AsFloat (p[1]);
		break;

	case PGPU_OP_LINE_WIDTH: {
		float f = AsFloat (p[0]);
		if (!(f > 0.0f))
		{
			return PGPU_ERR_ENUM;
		}
		S.fLineWidth = f < 1.0f ? 1.0f : f > 32.0f ? 32.0f : f;	// vc4: 1 .. 32
		} break;

	// transform, lighting, fog, current values

	case PGPU_OP_LOAD_MATRIX: {
		float *pMatrix =   p[0] == PGPU_MODELVIEW ? S.Modelview
				 : p[0] == PGPU_PROJECTION ? S.Projection
				 : p[0] == PGPU_TEXTURE ? S.Texture : nullptr;
		if (!pMatrix)
		{
			return PGPU_ERR_ENUM;
		}
		memcpy (pMatrix, &p[1], 16 * sizeof (float));		// words -> floats
		} break;

	case PGPU_OP_LIGHT: {
		if (p[0] > 3)
		{
			return PGPU_ERR_ENUM;
		}
		TLight &L = S.Lights[p[0]];
		memcpy (L.Position, &p[1], 4 * sizeof (float));
		SetColor (L.Ambient, p[5]);
		SetColor (L.Diffuse, p[6]);
		SetColor (L.Specular, p[7]);
		memcpy (L.Attenuation, &p[8], 3 * sizeof (float));
		} break;

	case PGPU_OP_MATERIAL:
		SetColor (S.MatAmbient, p[0]);
		SetColor (S.MatDiffuse, p[1]);
		SetColor (S.MatSpecular, p[2]);
		SetColor (S.MatEmission, p[3]);
		S.fShininess = AsFloat (p[4]);
		break;

	case PGPU_OP_LIGHT_MODEL:
		SetColor (S.LightModelAmbient, p[0]);
		S.bTwoSide = p[1] != 0;
		break;

	case PGPU_OP_FOG:
		if (p[0] > 2)
		{
			return PGPU_ERR_ENUM;
		}
		S.nFogMode = p[0];
		SetColor (S.FogColor, p[1]);
		S.fFogStart = AsFloat (p[2]);
		S.fFogEnd = AsFloat (p[3]);
		S.fFogDensity = AsFloat (p[4]);
		break;

	case PGPU_OP_SHADE_MODEL:
		if (p[0] > 1)
		{
			return PGPU_ERR_ENUM;
		}
		S.nShadeModel = p[0];
		break;

	case PGPU_OP_COLOR:
		SetColor (S.Color, p[0]);
		break;

	case PGPU_OP_NORMAL:
		memcpy (S.Normal, p, 3 * sizeof (float));
		break;

	case PGPU_OP_TEXCOORD:
		memcpy (S.TexCoord, p, 2 * sizeof (float));
		break;

	// arrays and drawing

	case PGPU_OP_ARRAY: {
		if (p[0] > PGPU_ATTR_TEXCOORD || p[4] < 1 || p[4] > 4 || p[5] >= TYPES)
		{
			return PGPU_ERR_ENUM;
		}
		if (p[1] > MaxBuffers)
		{
			*pDetail = p[1];
			return PGPU_ERR_ID;
		}
		TArray &A = m_Arrays[p[0]];
		A.nBuffer = p[1];
		A.nOffset = p[2];
		A.nSize = p[4];
		A.nType = p[5];
		A.nStride = p[3] ? p[3] : A.nSize * TypeBytes[A.nType];
		} break;

	case PGPU_OP_ARRAYS_ENABLE:
		if (p[0] & ~0xFu)
		{
			return PGPU_ERR_ENUM;
		}
		m_nArraysEnabled = p[0];
		break;

	case PGPU_OP_DRAW_ARRAYS:
		if (m_nProgram)
		{
			return ProgramDraw (p[0], p[1], p[2], FALSE, 0, 0, 0, pDetail);
		}
		return DrawArrays (p[0], p[1], p[2]);

	case PGPU_OP_DRAW_ELEMENTS:
		if (m_nProgram)
		{
			return ProgramDraw (p[0], 0, p[1], TRUE, p[2], p[3], p[4], pDetail);
		}
		return DrawElements (p[0], p[1], p[2], p[3], p[4]);

	case PGPU_OP_DRAW_INLINE:
		if (m_nProgram)
		{
			return PGPU_ERR_ENUM;		// not with programs
		}
		return DrawInline (p, nLength);

	// programs

	case PGPU_OP_PROGRAM_CREATE:
		*pDetail = p[0];
		return m_Programs.Create (p[0], p[1]);

	case PGPU_OP_PROGRAM_DATA:
		if (nLength < 2)
		{
			return PGPU_ERR_LENGTH;
		}
		*pDetail = p[0];
		return m_Programs.Data (p[0], p[1], p + 2, nLength - 2, pDetail);

	case PGPU_OP_PROGRAM_DELETE:
		*pDetail = p[0];
		if (p[0] == m_nProgram)
		{
			m_nProgram = 0;
		}
		return m_Programs.Delete (p[0]);

	case PGPU_OP_USE_PROGRAM:
		if (p[0] != 0 && !m_Programs.Get (p[0]))
		{
			*pDetail = p[0];
			return p[0] > CPrograms::MaxPrograms ? PGPU_ERR_ID : PGPU_ERR_OBJECT;
		}
		m_nProgram = p[0];
		break;

	case PGPU_OP_PROGRAM_UNIFORM:
		if (nLength < 2)
		{
			return PGPU_ERR_LENGTH;
		}
		*pDetail = p[0];
		return m_Programs.Uniform (p[0], p[1], p + 2, nLength - 2);

	case PGPU_OP_PROGRAM_SAMPLER:
		*pDetail = p[0];
		return m_Programs.Sampler (p[0], p[1], p[2]);

	case PGPU_OP_VERTEX_ATTRIB:
		if (p[0] >= PGPU_MAX_ATTRIBUTES)
		{
			return PGPU_ERR_LIMIT;
		}
		memcpy (m_GenericValue[p[0]], p + 1, 4 * sizeof (float));
		break;

	case PGPU_OP_ATTRIB_ARRAY: {
		if (p[0] >= PGPU_MAX_ATTRIBUTES)
		{
			return PGPU_ERR_LIMIT;
		}
		if (p[4] < 1 || p[4] > 4 || p[5] >= TYPES)
		{
			return PGPU_ERR_ENUM;
		}
		if (p[1] > MaxBuffers)
		{
			*pDetail = p[1];
			return PGPU_ERR_ID;
		}
		TGenericArray &A = m_Generic[p[0]];
		A.nBuffer = p[1];
		A.nOffset = p[2];
		A.nStride = p[3];
		A.nSize = p[4];
		A.nType = p[5];
		} break;

	case PGPU_OP_PROGRAM_DRAW_INLINE:
		return ProgramDrawInline (p, nLength, pDetail);

	case PGPU_OP_ATTRIBS_ENABLE:
		if (p[0] >> PGPU_MAX_ATTRIBUTES)
		{
			return PGPU_ERR_ENUM;
		}
		m_nGenericEnabled = p[0];
		break;
	}

	return 0;
}

u32 CCommands::BufferCreate (u32 nId, unsigned nSize)
{
	if (nId < 1 || nId > MaxBuffers)
	{
		return PGPU_ERR_ID;
	}

	BufferDelete (nId);

	if (nSize > MaxBufferBytes || m_nBufferBytes + nSize > MaxBufferBytes)
	{
		return PGPU_ERR_MEMORY;
	}

	TBuffer &B = m_Buffers[nId];
	B.pData = new u8[nSize ? nSize : 4];
	if (!B.pData)
	{
		return PGPU_ERR_MEMORY;
	}
	B.nSize = nSize;
	B.bUsed = FALSE;
	m_nBufferBytes += nSize;

	return 0;
}

// The fixed-function pipeline reads vertex data at draw time, but the V3D reads
// the buffers of program draws when the frame renders: a buffer used by a
// program draw of the current frame gets new storage (copy-on-write).
u32 CCommands::BufferData (const u32 *p, unsigned nLength)
{
	if (nLength < 3)
	{
		return PGPU_ERR_LENGTH;
	}

	u32 nId = p[0];
	unsigned nOffset = p[1], nBytes = p[2];

	if (nId < 1 || nId > MaxBuffers)
	{
		return PGPU_ERR_ID;
	}
	TBuffer &B = m_Buffers[nId];
	if (!B.pData)
	{
		return PGPU_ERR_OBJECT;
	}
	if ((nBytes + 3) / 4 != nLength - 3)		// any offset and length (glBufferSubData)
	{
		return PGPU_ERR_LENGTH;
	}
	if (nOffset > B.nSize || nBytes > B.nSize - nOffset)
	{
		return PGPU_ERR_LIMIT;
	}

	if (B.bUsed)
	{
		u8 *pNew = new u8[B.nSize ? B.nSize : 4];
		if (!pNew)
		{
			return PGPU_ERR_MEMORY;
		}
		memcpy (pNew, B.pData, B.nSize);
		RetireBuffer (&B);
		B.pData = pNew;
		CV3D::Flush (B.pData, B.nSize);
	}

	memcpy (B.pData + nOffset, p + 3, nBytes);
	CV3D::Flush (B.pData + nOffset, nBytes);	// the V3D reads it

	return 0;
}

void CCommands::RetireBuffer (TBuffer *pBuffer)
{
	if (m_nRetiredBuffers < MaxRetiredBuffers)
	{
		m_RetiredBuffers[m_nRetiredBuffers++] = pBuffer->pData;
	}
	// else: too many in one frame, leak rather than corrupt

	pBuffer->pData = nullptr;
	pBuffer->bUsed = FALSE;
}

u32 CCommands::BufferDelete (u32 nId)
{
	TBuffer &B = m_Buffers[nId];
	if (!B.pData)
	{
		return PGPU_ERR_OBJECT;
	}

	if (B.bUsed)
	{
		RetireBuffer (&B);
	}
	else
	{
		delete [] B.pData;
		B.pData = nullptr;
	}
	m_nBufferBytes -= B.nSize;
	B.nSize = 0;

	return 0;
}

void CCommands::DefaultInput (TInputVertex *pVertex) const
{
	Set4 (pVertex->Position, 0, 0, 0, 1);
	memcpy (pVertex->Color, m_State.Color, sizeof pVertex->Color);
	memcpy (pVertex->Normal, m_State.Normal, sizeof pVertex->Normal);
	memcpy (pVertex->TexCoord, m_State.TexCoord, sizeof pVertex->TexCoord);
}

// fetch vertices nFirst .. nFirst+nCount-1 from the enabled arrays into m_pInput[0 ..]
u32 CCommands::FetchVertices (unsigned nFirst, unsigned nCount)
{
	if (!(m_nArraysEnabled & PGPU_ATTRIB (PGPU_ATTR_POSITION)))
	{
		return PGPU_ERR_OBJECT;			// nothing to draw from
	}
	if (nCount > CGeometry::MaxVertices)
	{
		return PGPU_ERR_LIMIT;
	}

	for (unsigned i = 0; i < nCount; i++)
	{
		DefaultInput (&m_pInput[i]);
	}

	for (unsigned a = 0; a < 4; a++)
	{
		if (!(m_nArraysEnabled & (1 << a)))
		{
			continue;
		}

		const TArray &A = m_Arrays[a];
		const TBuffer &B = m_Buffers[A.nBuffer];
		if (A.nBuffer == 0 || !B.pData)
		{
			return PGPU_ERR_OBJECT;
		}

		unsigned nElement = A.nSize * TypeBytes[A.nType];
		if (nCount == 0)
		{
			continue;
		}
		u64 nEnd = A.nOffset + (u64) (nFirst + nCount - 1) * A.nStride + nElement;
		if (nEnd > B.nSize)
		{
			return PGPU_ERR_LIMIT;
		}

		unsigned nComponents = A.nSize;
		static const unsigned MaxComponents[4] = {4, 4, 3, 2};
		if (nComponents > MaxComponents[a])
		{
			nComponents = MaxComponents[a];
		}

		const u8 *pSource = B.pData + A.nOffset + nFirst * A.nStride;
		unsigned nTypeBytes = TypeBytes[A.nType];
		for (unsigned i = 0; i < nCount; i++, pSource += A.nStride)
		{
			TInputVertex &V = m_pInput[i];
			float *pDest =   a == PGPU_ATTR_POSITION ? V.Position
				       : a == PGPU_ATTR_COLOR ? V.Color
				       : a == PGPU_ATTR_NORMAL ? V.Normal : V.TexCoord;

			if (a == PGPU_ATTR_COLOR && nComponents == 3)
			{
				pDest[3] = 1.0f;
			}
			else if (a == PGPU_ATTR_TEXCOORD && nComponents == 1)
			{
				pDest[1] = 0.0f;
			}
			else if (a == PGPU_ATTR_NORMAL)
			{
				pDest[0] = pDest[1] = pDest[2] = 0.0f;
			}

			for (unsigned c = 0; c < nComponents; c++)
			{
				pDest[c] = Component (pSource + c * nTypeBytes, A.nType);
			}
		}
	}

	return 0;
}

u32 CCommands::Draw (u32 nMode, unsigned nVertices, const u32 *pIndices, unsigned nCount)
{
	u32 nError = m_Geometry.Draw (m_State, nMode, m_pInput, nVertices, pIndices, nCount);
	if (!nError)
	{
		m_bJobPending = TRUE;
	}

	return nError;
}

u32 CCommands::DrawArrays (u32 nMode, unsigned nFirst, unsigned nCount)
{
	if (nMode > PGPU_TRIANGLE_FAN)
	{
		return PGPU_ERR_ENUM;
	}

	u32 nError = FetchVertices (nFirst, nCount);
	if (nError)
	{
		return nError;
	}

	return Draw (nMode, nCount, nullptr, nCount);
}

u32 CCommands::DrawElements (u32 nMode, unsigned nCount, u32 nIndexType, u32 nBuffer, unsigned nOffset)
{
	if (nMode > PGPU_TRIANGLE_FAN || nIndexType > 1)
	{
		return PGPU_ERR_ENUM;
	}
	if (nBuffer < 1 || nBuffer > MaxBuffers)
	{
		return PGPU_ERR_ID;
	}
	const TBuffer &B = m_Buffers[nBuffer];
	if (!B.pData)
	{
		return PGPU_ERR_OBJECT;
	}
	if (nCount > CGeometry::MaxVertices)
	{
		return PGPU_ERR_LIMIT;
	}

	unsigned nIndexBytes = nIndexType ? 2 : 1;
	if ((u64) nOffset + (u64) nCount * nIndexBytes > B.nSize)
	{
		return PGPU_ERR_LIMIT;
	}

	// read the indices and fetch only the range they use
	const u8 *pSource = B.pData + nOffset;
	u32 nMin = 0xFFFFFFFF, nMax = 0;
	for (unsigned i = 0; i < nCount; i++)
	{
		u32 nIndex = nIndexType ? (u32) (pSource[2*i] | pSource[2*i + 1] << 8) : pSource[i];
		m_pIndices[i] = nIndex;
		if (nIndex < nMin) nMin = nIndex;
		if (nIndex > nMax) nMax = nIndex;
	}
	if (nCount == 0)
	{
		return 0;
	}

	for (unsigned i = 0; i < nCount; i++)
	{
		m_pIndices[i] -= nMin;
	}

	unsigned nVertices = nMax - nMin + 1;
	u32 nError = FetchVertices (nMin, nVertices);
	if (nError)
	{
		return nError;
	}

	return Draw (nMode, nVertices, m_pIndices, nCount);
}

u32 CCommands::DrawInline (const u32 *p, unsigned nLength)
{
	if (nLength < 3)
	{
		return PGPU_ERR_LENGTH;
	}

	u32 nMode = p[0];
	u32 nCount = p[1];
	u32 nMask = p[2];

	if (   nMode > PGPU_TRIANGLE_FAN
	    || !(nMask & PGPU_ATTRIB (PGPU_ATTR_POSITION))
	    || (nMask & ~0xFu))
	{
		return PGPU_ERR_ENUM;
	}

	unsigned nWords = 3;
	if (nMask & PGPU_ATTRIB (PGPU_ATTR_COLOR))	nWords += 1;
	if (nMask & PGPU_ATTRIB (PGPU_ATTR_NORMAL))	nWords += 3;
	if (nMask & PGPU_ATTRIB (PGPU_ATTR_TEXCOORD))	nWords += 2;

	if (nCount > PGPU_MAX_PAYLOAD || nLength != 3 + nCount * nWords)
	{
		return PGPU_ERR_LENGTH;
	}

	const u32 *v = p + 3;
	for (unsigned i = 0; i < nCount; i++)
	{
		TInputVertex &V = m_pInput[i];
		DefaultInput (&V);

		memcpy (V.Position, v, 3 * sizeof (float));
		v += 3;
		if (nMask & PGPU_ATTRIB (PGPU_ATTR_COLOR))
		{
			SetColor (V.Color, *v++);
		}
		if (nMask & PGPU_ATTRIB (PGPU_ATTR_NORMAL))
		{
			memcpy (V.Normal, v, 3 * sizeof (float));
			v += 3;
		}
		if (nMask & PGPU_ATTRIB (PGPU_ATTR_TEXCOORD))
		{
			memcpy (V.TexCoord, v, 2 * sizeof (float));
			v += 2;
		}
	}

	return Draw (nMode, nCount, nullptr, nCount);
}

void CCommands::EndFrame (u32 nFlags)
{
	// the bound framebuffer's job, then the panel: its back buffer gets the
	// frame (from the previous frame's image, if nothing was drawn or cleared)
	u32 nBound = m_nFramebuffer;
	if (nBound)
	{
		FlushJob (FALSE);
		m_nFramebuffer = 0;
		UpdateTarget ();
	}
	if (!nBound || !m_bPanelDrawn)
	{
		FlushJob (TRUE);
	}
	m_pRenderer->Present (&m_FrameStats);
	m_bPanelDrawn = FALSE;
	if (nBound)
	{
		m_nFramebuffer = nBound;
		UpdateTarget ();
	}

	const TRenderStats &R = m_FrameStats;
	m_nFrameNumber++;
	m_nTotalFrames++;
	m_nLastFrameUs = R.nRenderUs;

	m_Stats.nFrames++;
	m_Stats.nDraws += R.nDraws;
	m_Stats.nTriangles += R.nTriangles;
	m_Stats.nDropped += R.nDroppedTriangles;
	m_Stats.nRenderUs += R.nRenderUs;
	m_Stats.nPresentWaitUs += R.nPresentWaitUs;

	if (nFlags & PGPU_FRAME_REPLY)
	{
		u32 Done[4] = {m_nFrameNumber, R.nRenderUs, R.nDraws, R.nTriangles};
		Reply (PGPU_REPLY_FRAME_DONE, Done, 4);
	}

	memset (&m_FrameStats, 0, sizeof m_FrameStats);
}

// the render target's size and orientation in the state (the panel: rows top
// down, no alpha; a texture: GL row order, alpha)
void CCommands::UpdateTarget (void)
{
	TGLState &S = m_State;
	S.nTargetWidth = m_pRenderer->GetWidth ();
	S.nTargetHeight = m_pRenderer->GetHeight ();
	S.bFlipY = TRUE;
	S.bTargetAlpha = FALSE;

	if (m_nFramebuffer)
	{
		const TFramebuffer &F = m_Framebuffers[m_nFramebuffer];
		u32 nBus;
		unsigned nWidth, nHeight;
		boolean bT;
		if (m_Textures.GetRenderTarget (F.nTexture, F.nFace, &nBus, &nWidth, &nHeight, &bT))
		{
			S.nTargetWidth = nWidth;
			S.nTargetHeight = nHeight;
		}
		S.bFlipY = FALSE;
		S.bTargetAlpha = m_Textures.TargetHasAlpha (F.nTexture);	// else DST_ALPHA is 1
	}
}

// the texture is about to change on the ARM: draws into it that the bound
// framebuffer collected come first (GL order)
void CCommands::FlushIfTarget (u32 nTexture)
{
	if (m_nFramebuffer && m_Framebuffers[m_nFramebuffer].nTexture == nTexture)
	{
		FlushJob (FALSE);
	}
}

// render what the renderer collected into the bound target (bForce: also
// without draws or a clear)
void CCommands::FlushJob (boolean bForce)
{
	if (!bForce && !m_bJobPending && !m_pRenderer->GetDraws ())
	{
		return;
	}

	TRenderTarget T;
	u32 nLoadColor;
	boolean bLoadZS;
	if (!m_nFramebuffer)
	{
		u32 nPrevious;
		m_pRenderer->GetPanelTarget (&T, &nPrevious);
		if (!(m_State.nEnables & PGPU_CAP_DITHER))
		{
			T.nModeFlags |= MODE_BGR565_NO_DITHER;	// the state when the job renders
		}
		nLoadColor = m_bJobClearColor ? 0 : m_bPanelDrawn ? T.nColorBus : nPrevious;
		bLoadZS = !m_bJobClearZS && m_bPanelZSValid;
	}
	else
	{
		TFramebuffer &F = m_Framebuffers[m_nFramebuffer];
		boolean bT;
		if (!m_Textures.GetRenderTarget (F.nTexture, F.nFace, &T.nColorBus, &T.nWidth,
						 &T.nHeight, &bT))
		{
			m_pRenderer->DiscardFrame ();		// no colour buffer: nothing to render
			EndJob ();
			return;
		}
		// RGBA8888 in the texture's tiled layout (R in byte 0, as the tile buffer)
		T.nModeFlags = 1 << 2 | (bT ? 1 : 2) << 6;
		T.nLoadStore = 1 | (bT ? 1 : 2) << 4;
		T.nZSBus = 0;
		if (F.bDepthStencil)
		{
			unsigned nBytes = DEPTH_BUFFER_SIZE (T.nWidth, T.nHeight);
			TZSBuffer &ZS = ZSOf (F);
			if (ZS.nBytes != nBytes)
			{
				delete [] ZS.p;
				ZS.p = new u8[nBytes + 4096];
				ZS.nBytes = nBytes;
				ZS.bValid = FALSE;
			}
			T.nZSBus = CV3D::BusAddress ((void *) (((uintptr) ZS.p + 4095) & ~(uintptr) 4095));
		}
		nLoadColor = m_bJobClearColor ? 0 : T.nColorBus;
		bLoadZS = !m_bJobClearZS && F.bDepthStencil && ZSOf (F).bValid;
	}

	if (!m_pRenderer->RenderJob (T, nLoadColor, bLoadZS, m_JobClear, &m_FrameStats))
	{
		LOGERR ("V3D job failed");
	}

	if (!m_nFramebuffer)
	{
		m_bPanelDrawn = TRUE;
		m_bPanelZSValid = TRUE;
	}
	else
	{
		TFramebuffer &F = m_Framebuffers[m_nFramebuffer];
		if (F.bDepthStencil)
		{
			ZSOf (F).bValid = TRUE;
		}
		m_Textures.Invalidate (F.nTexture);	// written by the V3D
	}

	EndJob ();
}

// the fixed-function geometry found the frame full
void CCommands::JobFullHandler (void *pParam)
{
	static_cast<CCommands *> (pParam)->FlushJob (FALSE);
}

// the job has rendered: storage replaced during it can be freed
void CCommands::EndJob (void)
{
	m_Textures.EndFrame ();
	m_Programs.EndFrame ();
	for (unsigned i = 0; i < m_nRetiredBuffers; i++)
	{
		delete [] m_RetiredBuffers[i];
	}
	m_nRetiredBuffers = 0;
	for (unsigned i = 1; i <= MaxBuffers; i++)
	{
		m_Buffers[i].bUsed = FALSE;
	}

	m_bJobPending = m_bJobClearColor = m_bJobClearZS = FALSE;
}

// CLEAR: at the start of a job it is the job's clear; otherwise (or when
// scissored, masked or clearing only one of depth and stencil) it is drawn
// as a rectangle over the target
u32 CCommands::Clear (u32 nMask, u32 nColor, float fDepth, u8 nStencil)
{
	const TGLState &S = m_State;
	boolean bColor = !!(nMask & PGPU_CLEAR_COLOR);
	boolean bDepth = !!(nMask & PGPU_CLEAR_DEPTH);
	boolean bStencil = !!(nMask & PGPU_CLEAR_STENCIL);
	if (!bColor && !bDepth && !bStencil)
	{
		return 0;
	}

	boolean bZSValid =   !m_nFramebuffer ? m_bPanelZSValid
			   : m_Framebuffers[m_nFramebuffer].bDepthStencil
			     && ZSOf (m_Framebuffers[m_nFramebuffer]).bValid;
	boolean bFast =    !m_pRenderer->GetDraws ()
			&& !(S.nEnables & PGPU_CAP_SCISSOR_TEST)
			&& (!bColor || (S.nColorMask & 0xF) == 0xF)
			&& (!bDepth || S.bDepthMask)
			&& (!bStencil || (S.StencilWriteMask[0] & S.StencilWriteMask[1] & 0xFF) == 0xFF)
			&& (bDepth == bStencil || !bZSValid || m_bJobClearZS);
	if (bFast)
	{
		if (bColor)
		{
			m_bJobClearColor = TRUE;
			m_JobClear.nColor = nColor;
		}
		if (bDepth || bStencil)
		{
			if (!m_bJobClearZS)
			{
				m_JobClear.fDepth = 1.0f;
				m_JobClear.nStencil = 0;
			}
			m_bJobClearZS = TRUE;
			if (bDepth)
			{
				m_JobClear.fDepth = fDepth;
			}
			if (bStencil)
			{
				m_JobClear.nStencil = nStencil;
			}
		}
		m_bJobPending = TRUE;
		return 0;
	}

	// a rectangle over the target: identity matrices, the clear depth as z,
	// masks and scissor as set, stencil REPLACE with the clear value
	TGLState C = S;
	memset (C.Modelview, 0, sizeof C.Modelview);
	C.Modelview[0] = C.Modelview[5] = C.Modelview[10] = C.Modelview[15] = 1.0f;
	memcpy (C.Projection, C.Modelview, sizeof C.Modelview);
	memcpy (C.Texture, C.Modelview, sizeof C.Modelview);
	C.ViewportX = C.ViewportY = 0.0f;
	C.ViewportW = S.nTargetWidth;
	C.ViewportH = S.nTargetHeight;
	C.DepthNear = 0.0f;
	C.DepthFar = 1.0f;
	C.nEnables = S.nEnables & PGPU_CAP_SCISSOR_TEST;
	C.nColorMask = bColor ? S.nColorMask : 0;
	C.nShadeModel = 0;
	if (bDepth)
	{
		C.nEnables |= PGPU_CAP_DEPTH_TEST;
		C.nDepthFunc = PGPU_ALWAYS;
		C.bDepthMask = S.bDepthMask;
	}
	if (bStencil)
	{
		C.nEnables |= PGPU_CAP_STENCIL_TEST;
		for (unsigned f = 0; f < 2; f++)
		{
			C.StencilFunc[f] = PGPU_ALWAYS;
			C.StencilRef[f] = nStencil;
			C.StencilFail[f] = C.StencilZFail[f] = C.StencilZPass[f] = PGPU_REPLACE_OP;
		}
	}

	float fZ = (fDepth < 0.0f ? 0.0f : fDepth > 1.0f ? 1.0f : fDepth) * 2.0f - 1.0f;
	static const float Corners[4][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
	for (unsigned i = 0; i < 4; i++)
	{
		TInputVertex &V = m_pInput[i];
		memset (&V, 0, sizeof V);
		V.Position[0] = Corners[i][0];
		V.Position[1] = Corners[i][1];
		V.Position[2] = fZ;
		V.Position[3] = 1.0f;
		SetColor (V.Color, nColor);
	}
	u32 nError = m_Geometry.Draw (C, PGPU_TRIANGLE_FAN, m_pInput, 4, nullptr, 4);
	if (!nError)
	{
		m_bJobPending = TRUE;
	}
	return nError;
}

// pixels of the bound target (GL coordinates, rows bottom up) as RGBA8888;
// renders the collected draws first
u32 CCommands::ReadRect (s32 x, s32 y, unsigned nWidth, unsigned nHeight, u32 *pOut)
{
	FlushJob (FALSE);

	unsigned W = m_State.nTargetWidth, H = m_State.nTargetHeight;
	const u16 *pPanel = nullptr;
	u32 nTexture = 0;
	unsigned nFace = 0;
	if (!m_nFramebuffer)
	{
		// the frame so far, or the previous frame if nothing was rendered yet
		pPanel = m_bPanelDrawn ? m_pRenderer->GetBackBuffer () : m_pRenderer->GetLastFrame ();
		CV3D::Flush (pPanel, W * H * 2);		// written by the V3D
	}
	else
	{
		nTexture = m_Framebuffers[m_nFramebuffer].nTexture;
		nFace = m_Framebuffers[m_nFramebuffer].nFace;
	}

	for (unsigned row = 0; row < nHeight; row++)
	{
		for (unsigned i = 0; i < nWidth; i++)
		{
			s32 px = x + (s32) i, py = y + (s32) row;
			u32 nPixel = 0;
			if (px >= 0 && py >= 0 && (unsigned) px < W && (unsigned) py < H)
			{
				if (pPanel)
				{
					u16 v = pPanel[(H - 1 - py) * W + px];	// rows top down
					u32 r = v >> 11, g = (v >> 5) & 0x3F, b = v & 0x1F;
					nPixel =   (r << 3 | r >> 2) | (g << 2 | g >> 4) << 8
						 | (b << 3 | b >> 2) << 16 | 0xFFu << 24;
				}
				else
				{
					nPixel = m_Textures.ReadRGBA (nTexture, nFace, px, py);
					if (!m_Textures.TargetHasAlpha (nTexture))
					{
						nPixel |= 0xFFu << 24;		// RGB: alpha 1
					}
				}
			}
			pOut[row * nWidth + i] = nPixel;
		}
	}

	return 0;
}

u32 CCommands::FramebufferCreate (u32 nId, u32 nTexture, u32 nFlags)
{
	if (nId < 1 || nId > MaxFramebuffers)
	{
		return PGPU_ERR_ID;
	}
	unsigned nShared = PGPU_FRAMEBUFFER_SHARED_ZS (nFlags);
	if ((nFlags & ~(PGPU_FRAMEBUFFER_DEPTH_STENCIL | 0xFF00)) || nShared > MaxSharedZS)
	{
		return PGPU_ERR_ENUM;
	}
	if (nId == m_nFramebuffer)
	{
		FlushJob (FALSE);
	}

	TFramebuffer &F = m_Framebuffers[nId];
	FramebufferFree (&F);
	F.bValid = TRUE;
	F.nTexture = nTexture & 0xFFFF;
	F.nFace = nTexture >> 24;
	F.bDepthStencil = !!(nFlags & PGPU_FRAMEBUFFER_DEPTH_STENCIL);
	F.nSharedZS = F.bDepthStencil ? nShared : 0;
	if (nId == m_nFramebuffer)
	{
		UpdateTarget ();
	}

	return 0;
}

void CCommands::FramebufferFree (TFramebuffer *F)
{
	delete [] F->ZS.p;
	memset (F, 0, sizeof *F);
}

boolean CCommands::Reply (u8 uchOpcode, const u32 *pPayload, unsigned nLength)
{
	if (m_pHostLink)
	{
		return m_pHostLink->SendReply (uchOpcode, pPayload, nLength);
	}

	return m_pReceiver->SendReply (uchOpcode, pPayload, nLength);
}

void CCommands::Error (u32 nCode, u32 nOpcode, u32 nDetail)
{
	m_Stats.nErrors++;
	m_Stats.nLastError = nCode << 8 | nOpcode;
	m_nTotalErrors++;

	u32 Payload[3] = {nCode, nOpcode, nDetail};
	Reply (PGPU_REPLY_ERROR, Payload, 3);
}

TCommandStats CCommands::GetStats (void)
{
	TGeometryStats G = m_Geometry.GetStats ();
	m_Stats.nPrimitives = G.nPrimitives;
	m_Stats.nClipped = G.nClipped;
	m_Stats.nRejected = G.nRejected;

	TCommandStats Stats = m_Stats;
	memset (&m_Stats, 0, sizeof m_Stats);

	return Stats;
}

// ---- programs (docs/protocol.md 7.10) ---------------------------------------

u32 CCommands::ConfigBits (boolean bFaces) const
{
	const TGLState &S = m_State;
	u32 nBits = CFG_FORWARD | CFG_REVERSE;

	if (bFaces)
	{
		nBits |= CGeometry::ClockwiseBit (S);
		if (S.nEnables & PGPU_CAP_CULL_FACE)
		{
			if (S.nCullFace == PGPU_BACK || S.nCullFace == PGPU_FRONT_AND_BACK)
				nBits &= ~CFG_REVERSE;
			if (S.nCullFace == PGPU_FRONT || S.nCullFace == PGPU_FRONT_AND_BACK)
				nBits &= ~CFG_FORWARD;
		}
	}

	if (S.nEnables & PGPU_CAP_DEPTH_TEST)
	{
		nBits |= S.nDepthFunc << CFG_DEPTH_FUNC__SHIFT;
		if (S.bDepthMask)
		{
			nBits |= CFG_Z_UPDATE;
		}
	}
	else
	{
		nBits |= PGPU_ALWAYS << CFG_DEPTH_FUNC__SHIFT;	// no test, no depth writes
	}

	return nBits;
}

// the fragment shader ending needed: plain, or blending and colour mask from uniforms
u32 CCommands::BlendMode (void) const
{
	return   (m_State.nEnables & PGPU_CAP_BLEND) || (m_State.nColorMask & 0xF) != 0xF
	       ? PGPU_BLEND_GENERIC : PGPU_BLEND_PLAIN;
}

// viewport transform and clip window, in target pixels (the panel's rows top down)
boolean CCommands::GetViewport (TViewport *pVP) const
{
	const TGLState &S = m_State;
	float fHeight = S.nTargetHeight;
	float fHalfHeight = S.ViewportH * 0.5f;

	pVP->fHalfWidth = S.ViewportW * 0.5f;
	pVP->fCentreX = S.ViewportX + pVP->fHalfWidth;
	pVP->fCentreY = S.bFlipY ? fHeight - (S.ViewportY + fHalfHeight) : S.ViewportY + fHalfHeight;
	pVP->fScaleY = S.bFlipY ? -fHalfHeight : fHalfHeight;
	pVP->fZScale = (S.DepthFar - S.DepthNear) * 0.5f;
	pVP->fZOffset = (S.DepthFar + S.DepthNear) * 0.5f;

	// the hardware clips against a guard band: clip the rendering to the
	// viewport (and the panel)
	s32 x0 = (s32) S.ViewportX, x1 = (s32) (S.ViewportX + S.ViewportW);
	s32 y0 = (s32) S.ViewportY, y1 = (s32) (S.ViewportY + S.ViewportH);
	if (S.bFlipY)
	{
		y0 = (s32) (fHeight - (S.ViewportY + S.ViewportH));
		y1 = (s32) (fHeight - S.ViewportY);
	}
	s32 w = S.nTargetWidth, h = (s32) fHeight;
	if (x0 < 0) x0 = 0;
	if (y0 < 0) y0 = 0;
	if (x1 > w) x1 = w;
	if (y1 > h) y1 = h;
	if (x1 <= x0 || y1 <= y0)
	{
		return FALSE;
	}
	pVP->nClipX = x0;
	pVP->nClipY = y0;
	pVP->nClipWidth = x1 - x0;
	pVP->nClipHeight = y1 - y0;

	return TRUE;
}

// resolve a shader's uniform stream into the frame's uniform pool
// *pStorageBus: a copy of the uniform storage for this draw (0: not made yet)
u32 *CCommands::BuildUniforms (const TProgram *pProgram, const TProgramShader *pShader, u32 *pStorageBus,
			       const TViewport &rVP, u32 *pBus)
{
	u32 *pStream = m_pRenderer->AllocUniforms (pShader->nUniforms ? pShader->nUniforms : 1, pBus);
	if (!pStream)
	{
		return nullptr;
	}

	for (unsigned i = 0; i < pShader->nUniforms; i++)
	{
		u32 nKind = pShader->pUniforms[2 * i];
		u32 nData = pShader->pUniforms[2 * i + 1];
		u32 nValue = 0;
		float f;

		switch (nKind)
		{
		case PGPU_U_CONSTANT:		nValue = nData; break;
		case PGPU_U_UNIFORM:		nValue = pProgram->pUniforms[nData]; break;
		case PGPU_U_VIEWPORT_X_SCALE:	f = rVP.fHalfWidth * 16.0f; memcpy (&nValue, &f, 4); break;
		case PGPU_U_VIEWPORT_Y_SCALE:	f = rVP.fScaleY * 16.0f; memcpy (&nValue, &f, 4); break;
		case PGPU_U_VIEWPORT_Z_OFFSET:	memcpy (&nValue, &rVP.fZOffset, 4); break;
		case PGPU_U_VIEWPORT_Z_SCALE:	memcpy (&nValue, &rVP.fZScale, 4); break;
		case PGPU_U_UNIFORMS_ADDRESS:	nValue = *pBus; break;

		case PGPU_U_UBO0_ADDR:		// the TMU reads the storage when the frame renders
			if (!*pStorageBus)
			{
				u8 *pCopy = m_pRenderer->AllocData (pProgram->nUniformWords * 4, pStorageBus);
				if (!pCopy)
				{
					return nullptr;
				}
				memcpy (pCopy, pProgram->pUniforms, pProgram->nUniformWords * 4);
			}
			nValue = *pStorageBus + nData;
			break;

		case PGPU_U_TEXTURE_CONFIG_P0:
		case PGPU_U_TEXTURE_CONFIG_P1:
		case PGPU_U_TEXTURE_CONFIG_P2: {
			// P2 (cube maps): data is the sampler | bias/LOD flag << 16
			CTextures::TConfig Tex;
			u32 nTexture = m_TextureUnit[pProgram->SamplerUnit[nData & 0xFFFF]];
			if (!m_Textures.Use (nTexture, &Tex))
			{
				m_Textures.UseFallback (&Tex);		// reads (0, 0, 0, 1)
			}
			nValue =   nKind == PGPU_U_TEXTURE_CONFIG_P0 ? Tex.P0
				 : nKind == PGPU_U_TEXTURE_CONFIG_P1 ? Tex.P1
				 : Tex.P2 | ((nData >> 16) & 1);
			} break;

		case PGPU_U_STENCIL: {
			u32 W[3];
			CGeometry::StencilWords (m_State, W);
			nValue = W[nData];
			} break;

		case PGPU_U_BLEND:
			memcpy (&nValue, &m_BlendK[nData / 12][nData % 12], 4);
			break;

		case PGPU_U_FB_Y_TRANSFORM: {	// as Mesa: window (rows top down) or texture target
			float H = m_State.nTargetHeight;
			const float Flipped[4] = {-1.0f, H, 1.0f, 0.0f}, Straight[4] = {1.0f, 0.0f, -1.0f, H};
			memcpy (&nValue, m_State.bFlipY ? &Flipped[nData] : &Straight[nData], 4);
			} break;

		case PGPU_U_DEPTH_RANGE: {
			const float Values[4] = {m_State.DepthNear, m_State.DepthFar,
						 m_State.DepthFar - m_State.DepthNear, 1.0f};
			memcpy (&nValue, &Values[nData], 4);
			} break;

		case PGPU_U_POINT_Y_TRANSFORM: {	// upper-left origin (flipped when rows go top down)
			// (Mesa 26's vc4 compiles the origin into the FS instead:
			// PGPU_VK_TEXTURE_TARGET variants)
			const float Flipped[4] = {-1.0f, 1.0f, 0.0f, 0.0f}, Straight[4] = {1.0f, 0.0f, 0.0f, 0.0f};
			memcpy (&nValue, m_State.bFlipY ? &Flipped[nData] : &Straight[nData], 4);
			} break;

		default:			// first level: 0
			nValue = 0;
			break;
		}

		pStream[i] = nValue;
	}

	return pStream;
}

// payload: mode, vertices, attribute mask, index count (0: not indexed),
// index type, then per attribute in the mask (ascending): format (type |
// size << 8) and the vertices' values tightly packed (padded to a word),
// then the indices (padded to a word)
u32 CCommands::ProgramDrawInline (const u32 *p, unsigned nLength, u32 *pDetail)
{
	if (nLength < 5)
	{
		return PGPU_ERR_LENGTH;
	}
	if (!m_nProgram)
	{
		return PGPU_ERR_ENUM;			// programs only
	}
	TProgram *pProgram = m_Programs.Get (m_nProgram);
	if (!pProgram)
	{
		*pDetail = m_nProgram;
		return PGPU_ERR_OBJECT;
	}

	u32 nMode = p[0];
	unsigned nVertices = p[1];
	u32 nMask = p[2];
	unsigned nIndices = p[3];
	u32 nIndexType = p[4];
	if (   nVertices > MAX_PROGRAM_VERTICES || nIndices > MAX_PROGRAM_VERTICES
	    || (nMask >> pProgram->nAttributes) || nIndexType > 1)
	{
		return nVertices > MAX_PROGRAM_VERTICES || nIndices > MAX_PROGRAM_VERTICES
		       ? PGPU_ERR_LIMIT : PGPU_ERR_ENUM;
	}

	TInlineData Inline;
	memset (&Inline, 0, sizeof Inline);
	Inline.nMask = nMask;
	unsigned w = 5;
	for (unsigned i = 0; i < pProgram->nAttributes; i++)
	{
		if (!(nMask & (1 << i)))
		{
			continue;
		}
		if (w >= nLength)
		{
			return PGPU_ERR_LENGTH;
		}
		u32 nFormat = p[w++];
		if (   PGPU_ATTR_TYPE (nFormat) >= TYPES
		    || PGPU_ATTR_SIZE (nFormat) < 1 || PGPU_ATTR_SIZE (nFormat) > 4)
		{
			*pDetail = i;
			return PGPU_ERR_ENUM;
		}
		Inline.Format[i] = nFormat;		// converted if not the program's
		unsigned nBytes = PGPU_ATTR_SIZE (nFormat) * TypeBytes[PGPU_ATTR_TYPE (nFormat)] * nVertices;
		unsigned nWords = (nBytes + 3) / 4;
		if (w + nWords > nLength)
		{
			return PGPU_ERR_LENGTH;
		}
		Inline.pAttribute[i] = (const u8 *) (p + w);
		w += nWords;
	}

	if (nIndices)
	{
		Inline.nIndexBytes = nIndices * (nIndexType ? 2 : 1);
		Inline.pIndices = (const u8 *) (p + w);
		w += (Inline.nIndexBytes + 3) / 4;
	}
	if (w != nLength)
	{
		return PGPU_ERR_LENGTH;
	}

	return nIndices ? ProgramDraw (nMode, 0, nIndices, TRUE, nIndexType, 0, 0, pDetail, &Inline, nVertices)
			: ProgramDraw (nMode, 0, nVertices, FALSE, 0, 0, 0, pDetail, &Inline, nVertices);
}

u32 CCommands::ProgramDraw (u32 nMode, unsigned nFirst, unsigned nCount,
			   boolean bIndexed, u32 nIndexType, u32 nIndexBuffer, unsigned nIndexOffset,
			   u32 *pDetail, const TInlineData *pInline, unsigned nVertices)
{
	TProgram *pProgram = m_Programs.Get (m_nProgram);
	if (!pProgram)
	{
		*pDetail = m_nProgram;
		return PGPU_ERR_OBJECT;
	}
	if (nMode > PGPU_TRIANGLE_FAN || (bIndexed && nIndexType > 1))
	{
		return PGPU_ERR_ENUM;
	}
	if (nCount > MAX_PROGRAM_VERTICES)
	{
		return PGPU_ERR_LIMIT;
	}
	unsigned nPrim =   nMode == PGPU_POINTS ? PGPU_PRIM_POINTS
			 : nMode <= PGPU_LINE_STRIP ? PGPU_PRIM_LINES : PGPU_PRIM_TRIANGLES;
	const TProgramVariant *pVariant = CPrograms::FindVariant (pProgram, nPrim, BlendMode (),
								  !m_State.bFlipY);
	if (!pVariant)
	{
		*pDetail = 2;
		return PGPU_ERR_PROGRAM;
	}

	if (nCount == 0)
	{
		return 0;
	}

	TViewport VP;
	if (!GetViewport (&VP))
	{
		return 0;			// nothing visible
	}

	// shader record and attribute records
	unsigned nAttributes = pProgram->nAttributes;
	u32 nRecordBus;
	u8 *pRecord = m_pRenderer->AllocRecord (SHADER_RECORD_BYTES + nAttributes * ATTRIBUTE_RECORD_BYTES,
						&nRecordBus);
	if (!pRecord)
	{
		return PGPU_ERR_MEMORY;
	}

	// the vertices an indexed draw uses, for arrays that must be converted
	unsigned nMinIndex = 0, nMaxUsed = 0;
	boolean bRangeKnown = FALSE;
	auto IndexRange = [&] (void) -> boolean
	{
		if (bRangeKnown)
		{
			return TRUE;
		}
		const u8 *pIndices;
		if (pInline)
		{
			pIndices = pInline->pIndices;
		}
		else
		{
			const TBuffer &IB = m_Buffers[nIndexBuffer];
			if (   nIndexBuffer < 1 || nIndexBuffer > MaxBuffers || !IB.pData
			    || (u64) nIndexOffset + (u64) nCount * (nIndexType ? 2 : 1) > IB.nSize)
			{
				return FALSE;
			}
			pIndices = IB.pData + nIndexOffset;
		}
		nMinIndex = 0xFFFFFFFF;
		for (unsigned k = 0; k < nCount; k++)
		{
			unsigned n = nIndexType ? pIndices[2 * k] | pIndices[2 * k + 1] << 8 : pIndices[k];
			if (n < nMinIndex) nMinIndex = n;
			if (n > nMaxUsed) nMaxUsed = n;
		}
		bRangeKnown = TRUE;
		return TRUE;
	};

	u32 nMaxIndex = 0xFFFF;
	u8 *pAttribute = pRecord + SHADER_RECORD_BYTES;
	for (unsigned i = 0; i < nAttributes; i++, pAttribute += ATTRIBUTE_RECORD_BYTES)
	{
		u32 nType = PGPU_ATTR_TYPE (pProgram->Attributes[i]);
		unsigned nSize = PGPU_ATTR_SIZE (pProgram->Attributes[i]);
		unsigned nBytes = nSize * TypeBytes[nType];
		u32 nAddress;
		unsigned nStride;

		if (pInline && (pInline->nMask & (1 << i)))
		{
			// vertex data from the packet into the frame's vertex pool,
			// converted to the program's format if needed
			u32 nSrcType = PGPU_ATTR_TYPE (pInline->Format[i]);
			unsigned nSrcSize = PGPU_ATTR_SIZE (pInline->Format[i]);
			if (nSrcType == nType && nSrcSize == nSize)
			{
				u8 *pData = m_pRenderer->AllocData (nVertices * nBytes, &nAddress);
				if (!pData)
				{
					return PGPU_ERR_MEMORY;
				}
				memcpy (pData, pInline->pAttribute[i], nVertices * nBytes);
			}
			else if (!ConvertArray (m_pRenderer, pInline->pAttribute[i], nSrcSize * TypeBytes[nSrcType],
						nSrcType, nSrcSize, nVertices, nType, nSize, &nAddress))
			{
				return PGPU_ERR_MEMORY;
			}
			nStride = nBytes;
			if (nStride > 255)
			{
				return PGPU_ERR_LIMIT;
			}
			if (bIndexed && nVertices - 1 < nMaxIndex)
			{
				nMaxIndex = nVertices - 1;
			}
		}
		else if (m_nGenericEnabled & (1 << i))
		{
			const TGenericArray &A = m_Generic[i];
			TBuffer &B = m_Buffers[A.nBuffer];
			if (A.nBuffer == 0 || !B.pData)
			{
				*pDetail = A.nBuffer;
				return PGPU_ERR_OBJECT;
			}
			unsigned nSrcBytes = A.nSize * TypeBytes[A.nType];
			nStride = A.nStride ? A.nStride : nSrcBytes;
			if (A.nOffset > B.nSize || nSrcBytes > B.nSize - A.nOffset)
			{
				return PGPU_ERR_LIMIT;
			}

			if (A.nType != nType || A.nSize != nSize || nStride > 255)
			{
				// not the format the program was compiled for (or a stride
				// the V3D can't do): convert the vertices used
				unsigned nLast = (B.nSize - A.nOffset - nSrcBytes) / (nStride ? nStride : 1);
				unsigned nFirstUsed = nFirst, nUsed = nCount;
				if (bIndexed)
				{
					if (!IndexRange ())
					{
						return PGPU_ERR_LIMIT;
					}
					nFirstUsed = nMinIndex;
					nUsed = nMaxUsed - nMinIndex + 1;
				}
				if (nFirstUsed + nUsed - 1 > nLast)
				{
					return PGPU_ERR_LIMIT;
				}
				u32 nBus;
				if (!ConvertArray (m_pRenderer, B.pData + A.nOffset + nFirstUsed * nStride, nStride,
						   A.nType, A.nSize, nUsed, nType, nSize, &nBus))
				{
					return PGPU_ERR_MEMORY;
				}
				// the V3D adds index * stride: for indexed draws, the converted
				// data starts at the smallest index
				nAddress = bIndexed ? nBus - nMinIndex * nBytes : nBus;
				nStride = nBytes;
				if (bIndexed && nMaxUsed < nMaxIndex)
				{
					nMaxIndex = nMaxUsed;
				}
				goto record;
			}

			unsigned nLast = (B.nSize - A.nOffset - nBytes) / nStride;	// last vertex in the buffer
			if (bIndexed)
			{
				if (nLast < nMaxIndex)
				{
					nMaxIndex = nLast;
				}
				nAddress = CV3D::BusAddress (B.pData + A.nOffset);
			}
			else
			{
				if (nFirst + nCount - 1 > nLast)
				{
					return PGPU_ERR_LIMIT;
				}
				// the first vertex goes into the address (GFXH-515: the
				// binner's vertex indices are 16 bits)
				nAddress = CV3D::BusAddress (B.pData + A.nOffset + nFirst * nStride);
			}
			B.bUsed = TRUE;
		}
		else
		{
			// the current value, in the program's format
			u8 *pData = m_pRenderer->AllocData (16, &nAddress);
			if (!pData)
			{
				return PGPU_ERR_MEMORY;
			}
			memset (pData, 0, 16);
			ConvertValue (m_GenericValue[i], nType, nSize, pData);
			nStride = 0;
		}

	record:
		CControlList Rec (pAttribute, ATTRIBUTE_RECORD_BYTES);
		Rec.Add32 (nAddress);
		Rec.Add8 (nBytes - 1);
		Rec.Add8 (nStride);
		Rec.Add8 (pVariant->pVS->VattrOffsets[i]);
		Rec.Add8 (pVariant->pCS->VattrOffsets[i]);
	}

	u32 nIndexBus = 0;
	if (bIndexed && pInline)
	{
		u8 *pData = m_pRenderer->AllocData (pInline->nIndexBytes, &nIndexBus);
		if (!pData)
		{
			return PGPU_ERR_MEMORY;
		}
		memcpy (pData, pInline->pIndices, pInline->nIndexBytes);
	}
	else if (bIndexed)
	{
		if (nIndexBuffer < 1 || nIndexBuffer > MaxBuffers)
		{
			*pDetail = nIndexBuffer;
			return PGPU_ERR_ID;
		}
		TBuffer &B = m_Buffers[nIndexBuffer];
		if (!B.pData)
		{
			*pDetail = nIndexBuffer;
			return PGPU_ERR_OBJECT;
		}
		unsigned nIndexBytes = nIndexType ? 2 : 1;
		if (   (nIndexOffset & (nIndexBytes - 1))
		    || (u64) nIndexOffset + (u64) nCount * nIndexBytes > B.nSize)
		{
			return PGPU_ERR_LIMIT;
		}
		nIndexBus = CV3D::BusAddress (B.pData + nIndexOffset);
		B.bUsed = TRUE;
	}

	// uniform streams
	CGeometry::BlendCoefficients (m_State, m_State.bTargetAlpha, m_BlendK);
	u32 nFSUniforms, nVSUniforms, nCSUniforms;
	u32 nStorageBus = 0;
	if (   !BuildUniforms (pProgram, pVariant->pFS, &nStorageBus, VP, &nFSUniforms)
	    || !BuildUniforms (pProgram, pVariant->pVS, &nStorageBus, VP, &nVSUniforms)
	    || !BuildUniforms (pProgram, pVariant->pCS, &nStorageBus, VP, &nCSUniforms))
	{
		return PGPU_ERR_MEMORY;
	}

	u32 nFSInfo = pVariant->pFS->nInfo;
	u32 nVSInfo = pVariant->pVS->nInfo;
	u32 nCSInfo = pVariant->pCS->nInfo;
	CControlList Rec (pRecord, SHADER_RECORD_BYTES);
	Rec.Add16 (  (nFSInfo & PGPU_SH_FS_THREADED ? 0 : SR_FS_SINGLE_THREADED)
		   | (pVariant->nKey & PGPU_VK_POINT_SIZE ? SR_POINT_SIZE : 0)
		   | SR_ENABLE_CLIPPING);
	Rec.Add8 (0);					// FS number of uniforms (unused)
	Rec.Add8 (PGPU_SH_FS_VARYINGS (nFSInfo));
	Rec.Add32 (pVariant->pFS->nCodeBus);
	Rec.Add32 (nFSUniforms);
	Rec.Add16 (0);					// VS number of uniforms (unused)
	Rec.Add8 (PGPU_SH_ATTR_SELECT (nVSInfo));
	Rec.Add8 (PGPU_SH_ATTR_SIZE (nVSInfo));
	Rec.Add32 (pVariant->pVS->nCodeBus);
	Rec.Add32 (nVSUniforms);
	Rec.Add16 (0);					// CS number of uniforms (unused)
	Rec.Add8 (PGPU_SH_ATTR_SELECT (nCSInfo));
	Rec.Add8 (PGPU_SH_ATTR_SIZE (nCSInfo));
	Rec.Add32 (pVariant->pCS->nCodeBus);
	Rec.Add32 (nCSUniforms);

	TGLDraw G;
	G.State.nConfigBits = ConfigBits (nPrim == PGPU_PRIM_TRIANGLES);
	CGeometry::GetDrawState (m_State, nPrim == PGPU_PRIM_TRIANGLES, &G.State);
	// the hardware clips against a guard band: clip the rendering to the viewport too
	unsigned x0 = VP.nClipX > G.State.nClipX ? VP.nClipX : G.State.nClipX;
	unsigned y0 = VP.nClipY > G.State.nClipY ? VP.nClipY : G.State.nClipY;
	unsigned x1 = VP.nClipX + VP.nClipWidth, y1 = VP.nClipY + VP.nClipHeight;
	if (G.State.nClipX + G.State.nClipWidth < x1) x1 = G.State.nClipX + G.State.nClipWidth;
	if (G.State.nClipY + G.State.nClipHeight < y1) y1 = G.State.nClipY + G.State.nClipHeight;
	if (x1 <= x0 || y1 <= y0)
	{
		return 0;			// scissored away
	}
	G.State.nClipX = x0;
	G.State.nClipY = y0;
	G.State.nClipWidth = x1 - x0;
	G.State.nClipHeight = y1 - y0;
	G.nRecordBus = nRecordBus;
	G.nAttributes = nAttributes;
	G.fCentreX = VP.fCentreX;
	G.fCentreY = VP.fCentreY;
	G.fHalfWidth = VP.fHalfWidth;
	G.fScaleY = VP.fScaleY;
	G.fZScale = VP.fZScale;
	G.fZOffset = VP.fZOffset;
	G.nMode = nMode;
	G.bIndexed = bIndexed;
	G.nCount = nCount;
	G.nIndexBus = nIndexBus;
	G.nIndexType = nIndexType;
	G.nMaxIndex = nMaxIndex;

	if (!m_pRenderer->AddGLDraw (G))
	{
		return PGPU_ERR_MEMORY;
	}

	m_Programs.Use (pProgram);
	m_bJobPending = TRUE;
	m_Stats.nProgramDraws++;

	return 0;
}

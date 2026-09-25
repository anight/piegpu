//
// commands.cpp
//
#include "commands.h"
#include <pgpu_protocol.h>
#include <circle/logger.h>
#include <circle/util.h>
#include <assert.h>

LOGMODULE ("commands");

#define VARIABLE		0xFFFF

// array component types (docs/protocol.md 10.5)
enum { TYPE_FLOAT, TYPE_SHORT, TYPE_SHORT_NORM, TYPE_UBYTE_NORM, TYPE_BYTE_NORM, TYPES };
static const unsigned TypeBytes[TYPES] = {4, 2, 2, 1, 1};

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

	default: {
		float f = (s8) p[0] / 127.0f;
		return f < -1.0f ? -1.0f : f;
		}
	}
}

CCommands::CCommands (CRenderer *pRenderer, CReceiver *pReceiver)
:	m_pRenderer (pRenderer),
	m_pReceiver (pReceiver),
	m_Geometry (pRenderer, &m_Textures),
	m_nBufferBytes (0),
	m_nClearColor (PGPU_RGBA (0, 0, 0, 255)),
	m_fClearDepth (1.0f),
	m_nFrameNumber (0),
	m_nLastFrameUs (0),
	m_nTotalFrames (0),
	m_nTotalErrors (0)
{
	memset (m_Buffers, 0, sizeof m_Buffers);
	m_pInput = new TInputVertex[CGeometry::MaxVertices];
	m_pIndices = new u32[CGeometry::MaxVertices];
	memset (&m_Stats, 0, sizeof m_Stats);

	DefaultState ();
	m_bFrameHasDraw = FALSE;
	m_bClearColor = FALSE;
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
	m_pRenderer->DiscardFrame ();
	m_Textures.EndFrame ();

	DefaultState ();
	m_bFrameHasDraw = FALSE;
	m_bClearColor = FALSE;
	m_nClearColor = PGPU_RGBA (0, 0, 0, 255);
	m_fClearDepth = 1.0f;
}

// docs/protocol.md 7.9
void CCommands::DefaultState (void)
{
	TGLState &S = m_State;
	memset (&S, 0, sizeof S);

	Identity (S.Modelview);
	Identity (S.Projection);
	Identity (S.Texture);

	S.ViewportW = m_pRenderer->GetWidth ();
	S.ViewportH = m_pRenderer->GetHeight ();
	S.DepthFar = 1.0f;

	S.nDepthFunc = PGPU_LESS;
	S.bDepthMask = TRUE;
	S.nBlendSrc = 1;		// ONE
	S.nBlendDst = 0;		// ZERO
	S.nCullFace = PGPU_BACK;
	S.nFrontFace = PGPU_CCW;
	S.nAlphaFunc = PGPU_ALWAYS;
	S.nColorMask = 0xF;

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
		{PGPU_OP_CLEAR, 3}, {PGPU_OP_FRAME_END, 1}, {PGPU_OP_VIEWPORT, 6},
		{PGPU_OP_BUFFER_CREATE, 2}, {PGPU_OP_BUFFER_DATA, VARIABLE}, {PGPU_OP_BUFFER_DELETE, 1},
		{PGPU_OP_TEXTURE_CREATE, 3}, {PGPU_OP_TEXTURE_DATA, VARIABLE}, {PGPU_OP_TEXTURE_PARAMS, 5},
		{PGPU_OP_TEXTURE_DELETE, 1}, {PGPU_OP_TEXTURE_BIND, 1}, {PGPU_OP_TEX_ENV, 2},
		{PGPU_OP_ENABLE, 1}, {PGPU_OP_DISABLE, 1}, {PGPU_OP_DEPTH_FUNC, 1},
		{PGPU_OP_DEPTH_MASK, 1}, {PGPU_OP_BLEND_FUNC, 2}, {PGPU_OP_CULL_FACE, 1},
		{PGPU_OP_FRONT_FACE, 1}, {PGPU_OP_ALPHA_FUNC, 2}, {PGPU_OP_COLOR_MASK, 1},
		{PGPU_OP_LOAD_MATRIX, 17}, {PGPU_OP_LIGHT, 11}, {PGPU_OP_MATERIAL, 5},
		{PGPU_OP_LIGHT_MODEL, 2}, {PGPU_OP_FOG, 5}, {PGPU_OP_SHADE_MODEL, 1},
		{PGPU_OP_COLOR, 1}, {PGPU_OP_NORMAL, 3}, {PGPU_OP_TEXCOORD, 2},
		{PGPU_OP_ARRAY, 6}, {PGPU_OP_ARRAYS_ENABLE, 1}, {PGPU_OP_DRAW_ARRAYS, 3},
		{PGPU_OP_DRAW_ELEMENTS, 5}, {PGPU_OP_DRAW_INLINE, VARIABLE},
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
		u32 Status[5] =
		{
			m_nTotalFrames,
			m_pReceiver->GetTotalCRCErrors (),
			m_nTotalErrors,
			m_pReceiver->GetFreeBytes (),
			m_nLastFrameUs
		};
		Reply (PGPU_REPLY_STATUS, Status, 5);
		} break;

	// frame

	case PGPU_OP_CLEAR:
		if (m_bFrameHasDraw)
		{
			return PGPU_ERR_CLEAR_AFTER_DRAW;
		}
		if (p[0] & ~(PGPU_CLEAR_COLOR | PGPU_CLEAR_DEPTH))
		{
			return PGPU_ERR_ENUM;
		}
		if (p[0] & PGPU_CLEAR_COLOR)
		{
			m_nClearColor = p[1];
			m_bClearColor = TRUE;
		}
		if (p[0] & PGPU_CLEAR_DEPTH)
		{
			m_fClearDepth = AsFloat (p[2]);
		}
		break;

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
		return m_Textures.Create (p[0], p[1] & 0xFFFF, p[1] >> 16, p[2]);

	case PGPU_OP_TEXTURE_DATA:
		if (nLength < 3)
		{
			return PGPU_ERR_LENGTH;
		}
		*pDetail = p[0];
		return m_Textures.Data (p[0], p[1] & 0xFFFF, p[1] >> 16, p[2] & 0xFFFF, p[2] >> 16,
					p + 3, nLength - 3);

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
		if (p[0] > 10 || p[1] > 9)
		{
			return PGPU_ERR_ENUM;
		}
		S.nBlendSrc = p[0];
		S.nBlendDst = p[1];
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
		return DrawArrays (p[0], p[1], p[2]);

	case PGPU_OP_DRAW_ELEMENTS:
		return DrawElements (p[0], p[1], p[2], p[3], p[4]);

	case PGPU_OP_DRAW_INLINE:
		return DrawInline (p, nLength);
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
	m_nBufferBytes += nSize;

	return 0;
}

// vertex data is consumed at draw time, so no copy-on-write is needed
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
	if ((nOffset | nBytes) & 3 || (nBytes + 3) / 4 != nLength - 3)
	{
		return PGPU_ERR_LENGTH;
	}
	if (nOffset > B.nSize || nBytes > B.nSize - nOffset)
	{
		return PGPU_ERR_LIMIT;
	}

	memcpy (B.pData + nOffset, p + 3, nBytes);

	return 0;
}

u32 CCommands::BufferDelete (u32 nId)
{
	TBuffer &B = m_Buffers[nId];
	if (!B.pData)
	{
		return PGPU_ERR_OBJECT;
	}

	delete [] B.pData;
	B.pData = nullptr;
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
		m_bFrameHasDraw = TRUE;
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
	// a frame without a colour CLEAR starts from the previous image (depth is
	// always cleared, to the last CLEAR depth value)
	TRenderStats R;
	if (!m_pRenderer->EndFrame (m_bClearColor, m_nClearColor, m_fClearDepth, &R))
	{
		LOGERR ("V3D job failed");
	}
	m_Textures.EndFrame ();

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

	m_bFrameHasDraw = FALSE;
	m_bClearColor = FALSE;
}

void CCommands::Reply (u8 uchOpcode, const u32 *pPayload, unsigned nLength)
{
	m_pReceiver->SendReply (uchOpcode, pPayload, nLength);
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

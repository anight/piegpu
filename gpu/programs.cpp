//
// programs.cpp
//
#include "programs.h"
#include <v3d.h>
#include <pgpu_protocol.h>
#include <circle/util.h>
#include <assert.h>

CPrograms::CPrograms (void)
:	m_nRetired (0)
{
	memset (m_Programs, 0, sizeof m_Programs);
}

CPrograms::~CPrograms (void)
{
}

void CPrograms::Reset (void)
{
	for (unsigned i = 1; i <= MaxPrograms; i++)
	{
		Free (&m_Programs[i], FALSE);
	}
}

// bKeepCode: the current frame may still run the code, free it after the frame
void CPrograms::Free (TProgram *p, boolean bKeepCode)
{
	if (p->pCode)
	{
		if (bKeepCode && p->bUsed && m_nRetired < MaxRetired)
		{
			m_Retired[m_nRetired++] = p->pCode;
		}
		else if (!(bKeepCode && p->bUsed))
		{
			delete [] p->pCode;
		}
		// else: too many retired in one frame, leak rather than corrupt
	}

	delete [] p->pBlob;
	delete [] p->pUniforms;
	delete [] p->pVariants;
	delete [] p->pShaders;
	memset (p, 0, sizeof *p);
}

u32 CPrograms::Create (u32 nId, unsigned nWords)
{
	if (nId < 1 || nId > MaxPrograms)
	{
		return PGPU_ERR_ID;
	}
	if (nWords < PGPU_PROGRAM_HEADER_WORDS || nWords > MaxBlobWords)
	{
		return PGPU_ERR_LIMIT;
	}

	TProgram *p = &m_Programs[nId];
	Free (p, TRUE);

	p->pBlob = new u32[nWords];
	if (!p->pBlob)
	{
		return PGPU_ERR_MEMORY;
	}
	p->nBlobWords = nWords;

	return 0;
}

u32 CPrograms::Data (u32 nId, unsigned nOffset, const u32 *pWords, unsigned nWords, u32 *pDetail)
{
	if (nId < 1 || nId > MaxPrograms)
	{
		return PGPU_ERR_ID;
	}
	TProgram *p = &m_Programs[nId];
	if (!p->pBlob || p->bReady)
	{
		return PGPU_ERR_OBJECT;
	}
	if (nOffset > p->nBlobWords || nWords > p->nBlobWords - nOffset)
	{
		return PGPU_ERR_LIMIT;
	}

	memcpy (p->pBlob + nOffset, pWords, nWords * 4);
	if (nOffset + nWords > p->nReceived)
	{
		p->nReceived = nOffset + nWords;
	}

	if (p->nReceived < p->nBlobWords)
	{
		return 0;
	}

	u32 nError = Load (p, pDetail);
	if (nError)
	{
		Free (p, TRUE);		// the id is left without a program
	}

	return nError;
}

// check the blob and load the code; *pDetail = the word where a check failed
u32 CPrograms::Load (TProgram *p, u32 *pDetail)
{
	const u32 *b = p->pBlob;
	unsigned n = p->nBlobWords;

#define CHECK(cond, word)	do { if (!(cond)) { *pDetail = (word); return PGPU_ERR_PROGRAM; } } while (0)

	CHECK (b[PGPU_PH_MAGIC] == PGPU_PROGRAM_MAGIC, PGPU_PH_MAGIC);
	CHECK (b[PGPU_PH_TOTAL_WORDS] == n, PGPU_PH_TOTAL_WORDS);

	u32 nCounts = b[PGPU_PH_COUNTS];
	p->nAttributes = nCounts & 0xFF;
	p->nSamplers = (nCounts >> 8) & 0xFF;
	p->nVariants = (nCounts >> 16) & 0xFF;
	p->nShaders = nCounts >> 24;
	CHECK (   p->nAttributes >= 1 && p->nAttributes <= PGPU_MAX_ATTRIBUTES
	       && p->nSamplers <= PGPU_MAX_SAMPLERS
	       && p->nVariants >= 1 && p->nShaders >= 1, PGPU_PH_COUNTS);

	p->nUniformWords = b[PGPU_PH_UNIFORM_WORDS];
	CHECK (p->nUniformWords <= MaxUniformWords, PGPU_PH_UNIFORM_WORDS);

	unsigned w = PGPU_PROGRAM_HEADER_WORDS;
	CHECK (w + p->nAttributes + 2 * p->nVariants <= n, w);
	for (unsigned i = 0; i < p->nAttributes; i++, w++)
	{
		CHECK (   PGPU_ATTR_TYPE (b[w]) <= PGPU_FIXED
		       && PGPU_ATTR_SIZE (b[w]) >= 1 && PGPU_ATTR_SIZE (b[w]) <= 4, w);
		p->Attributes[i] = b[w];
	}
	unsigned nVariantWords = w;
	w += 2 * p->nVariants;

	// shaders: count the code first, then allocate and copy
	p->pShaders = new TProgramShader[p->nShaders];
	unsigned nCodeWords = 0;
	unsigned nFirstShader = w;
	for (unsigned s = 0; s < p->nShaders; s++)
	{
		CHECK (w + PGPU_SHADER_HEADER_WORDS <= n, w);
		unsigned nInstructions = b[w + PGPU_SH_COUNTS] & 0xFFFF;
		unsigned nUniforms = b[w + PGPU_SH_COUNTS] >> 16;
		u32 nInfo = b[w + PGPU_SH_INFO];
		unsigned nStage = PGPU_SHADER_STAGE (nInfo);
		CHECK (nInstructions >= 1 && nStage <= PGPU_STAGE_CS, w);
		if (nStage != PGPU_STAGE_FS)
		{
			// only declared attributes, whole records inside the VPM data
			CHECK (!(PGPU_SH_ATTR_SELECT (nInfo) >> p->nAttributes), w + PGPU_SH_INFO);
		}

		TProgramShader &S = p->pShaders[s];
		S.nInfo = nInfo;
		for (unsigned k = 0; k < 8; k++)
		{
			S.VattrOffsets[k] = b[w + PGPU_SH_VATTR_OFFSETS + k / 4] >> (8 * (k % 4));
		}
		S.nUniforms = nUniforms;

		unsigned nCode = w + PGPU_SHADER_HEADER_WORDS;
		CHECK (nCode + 2 * nInstructions + 2 * nUniforms <= n, w);
		S.pUniforms = b + nCode + 2 * nInstructions;
		for (unsigned u = 0; u < nUniforms; u++)
		{
			u32 nKind = S.pUniforms[2 * u], nData = S.pUniforms[2 * u + 1];
			unsigned nWord = nCode + 2 * nInstructions + 2 * u;
			switch (nKind)
			{
			case PGPU_U_CONSTANT:
			case PGPU_U_VIEWPORT_X_SCALE:
			case PGPU_U_VIEWPORT_Y_SCALE:
			case PGPU_U_VIEWPORT_Z_OFFSET:
			case PGPU_U_VIEWPORT_Z_SCALE:
			case PGPU_U_UNIFORMS_ADDRESS:
				break;

			case PGPU_U_UNIFORM:
				CHECK (nData < p->nUniformWords, nWord);
				break;

			case PGPU_U_TEXTURE_CONFIG_P0:
			case PGPU_U_TEXTURE_CONFIG_P1:
			case PGPU_U_TEXTURE_CONFIG_P2:		// sampler | flag << 16
			case PGPU_U_TEXTURE_FIRST_LEVEL:
				CHECK ((nData & 0xFFFF) < p->nSamplers, nWord);
				break;

			case PGPU_U_BLEND:
				CHECK (nData < 48, nWord);
				break;

			case PGPU_U_STENCIL:
				CHECK (nData < 3, nWord);
				break;

			case PGPU_U_FB_Y_TRANSFORM:
			case PGPU_U_DEPTH_RANGE:
			case PGPU_U_POINT_Y_TRANSFORM:
				CHECK (nData < 4, nWord);
				break;

			default:
				CHECK (FALSE, nWord);
			}
		}

		nCodeWords += 2 * nInstructions;
		w = nCode + 2 * nInstructions + 2 * nUniforms;
	}
	CHECK (w == n, w);

	// variants
	p->pVariants = new TProgramVariant[p->nVariants];
	for (unsigned v = 0; v < p->nVariants; v++)
	{
		u32 nKey = b[nVariantWords + 2 * v];
		u32 nIndices = b[nVariantWords + 2 * v + 1];
		unsigned nFS = nIndices & 0xFF, nVS = (nIndices >> 8) & 0xFF, nCS = (nIndices >> 16) & 0xFF;
		CHECK (   PGPU_VK_PRIM (nKey) <= PGPU_PRIM_POINTS
		       && PGPU_VK_BLEND (nKey) <= PGPU_BLEND_GENERIC
		       && nFS < p->nShaders && nVS < p->nShaders && nCS < p->nShaders
		       && PGPU_SHADER_STAGE (p->pShaders[nFS].nInfo) == PGPU_STAGE_FS
		       && PGPU_SHADER_STAGE (p->pShaders[nVS].nInfo) == PGPU_STAGE_VS
		       && PGPU_SHADER_STAGE (p->pShaders[nCS].nInfo) == PGPU_STAGE_CS,
		       nVariantWords + 2 * v);

		TProgramVariant &V = p->pVariants[v];
		V.nKey = nKey;
		V.pFS = &p->pShaders[nFS];
		V.pVS = &p->pShaders[nVS];
		V.pCS = &p->pShaders[nCS];
	}

#undef CHECK

	// code into V3D-accessible memory (8-byte instructions)
	p->pCode = new u8[nCodeWords * 4 + 16];
	if (!p->pCode)
	{
		return PGPU_ERR_MEMORY;
	}
	u32 *pCode = (u32 *) (((uintptr) p->pCode + 15) & ~(uintptr) 15);
	w = nFirstShader;
	u32 *pNext = pCode;
	for (unsigned s = 0; s < p->nShaders; s++)
	{
		unsigned nInstructions = b[w + PGPU_SH_COUNTS] & 0xFFFF;
		unsigned nUniforms = b[w + PGPU_SH_COUNTS] >> 16;
		memcpy (pNext, b + w + PGPU_SHADER_HEADER_WORDS, nInstructions * 8);
		p->pShaders[s].nCodeBus = CV3D::BusAddress (pNext);
		pNext += 2 * nInstructions;
		w += PGPU_SHADER_HEADER_WORDS + 2 * nInstructions + 2 * nUniforms;
	}
	CV3D::Flush (pCode, nCodeWords * 4);

	p->pUniforms = new u32[p->nUniformWords + 1];
	memset (p->pUniforms, 0, (p->nUniformWords + 1) * 4);
	for (unsigned i = 0; i < PGPU_MAX_SAMPLERS; i++)
	{
		p->SamplerUnit[i] = i;
	}

	p->bReady = TRUE;

	return 0;
}

u32 CPrograms::Delete (u32 nId)
{
	if (nId < 1 || nId > MaxPrograms)
	{
		return PGPU_ERR_ID;
	}
	TProgram *p = &m_Programs[nId];
	if (!p->pBlob)
	{
		return PGPU_ERR_OBJECT;
	}

	Free (p, TRUE);

	return 0;
}

u32 CPrograms::Uniform (u32 nId, unsigned nOffset, const u32 *pWords, unsigned nWords)
{
	TProgram *p = Get (nId);
	if (!p)
	{
		return nId < 1 || nId > MaxPrograms ? PGPU_ERR_ID : PGPU_ERR_OBJECT;
	}
	if (nOffset > p->nUniformWords || nWords > p->nUniformWords - nOffset)
	{
		return PGPU_ERR_LIMIT;
	}

	// the uniform streams are built at draw time: no copy-on-write needed
	memcpy (p->pUniforms + nOffset, pWords, nWords * 4);

	return 0;
}

u32 CPrograms::Sampler (u32 nId, unsigned nSampler, unsigned nUnit)
{
	TProgram *p = Get (nId);
	if (!p)
	{
		return nId < 1 || nId > MaxPrograms ? PGPU_ERR_ID : PGPU_ERR_OBJECT;
	}
	if (nSampler >= PGPU_MAX_SAMPLERS || nUnit >= PGPU_MAX_TEXTURE_UNITS)
	{
		return PGPU_ERR_LIMIT;
	}

	p->SamplerUnit[nSampler] = nUnit;

	return 0;
}

TProgram *CPrograms::Get (u32 nId)
{
	if (nId < 1 || nId > MaxPrograms || !m_Programs[nId].bReady)
	{
		return nullptr;
	}

	return &m_Programs[nId];
}

const TProgramVariant *CPrograms::FindVariant (const TProgram *p, unsigned nPrim, unsigned nBlend)
{
	for (unsigned i = 0; i < p->nVariants; i++)
	{
		u32 nKey = p->pVariants[i].nKey;
		if (PGPU_VK_PRIM (nKey) == nPrim && PGPU_VK_BLEND (nKey) == nBlend)
		{
			return &p->pVariants[i];
		}
	}

	return nullptr;
}

void CPrograms::EndFrame (void)
{
	for (unsigned i = 0; i < m_nRetired; i++)
	{
		delete [] m_Retired[i];
	}
	m_nRetired = 0;

	for (unsigned i = 1; i <= MaxPrograms; i++)
	{
		m_Programs[i].bUsed = FALSE;
	}
}

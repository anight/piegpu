//
// programs.h
//
// Program objects (docs/protocol.md 7.10): blobs precompiled by tools/glslc
// (protocol/pgpu_program.h). A complete blob is checked and its QPU code is
// copied into V3D-accessible memory. Code of a program that is replaced or
// deleted while the current frame uses it is freed after the frame.
//
#ifndef _programs_h
#define _programs_h

#include <pgpu_program.h>
#include <circle/types.h>

struct TProgramShader
{
	u32 nInfo;			// PGPU_SH_INFO word
	u8 VattrOffsets[8];		// VS, CS: VPM byte offsets of the attributes
	u32 nCodeBus;
	unsigned nUniforms;
	const u32 *pUniforms;		// kind, data pairs (in the blob copy)
};

struct TProgramVariant
{
	u32 nKey;
	const TProgramShader *pFS, *pVS, *pCS;
};

struct TProgram
{
	boolean bReady;			// complete and checked
	unsigned nAttributes;
	u32 Attributes[PGPU_MAX_ATTRIBUTES];	// type | size << 8
	unsigned nSamplers;
	u8 SamplerUnit[PGPU_MAX_SAMPLERS];
	unsigned nUniformWords;
	u32 *pUniforms;			// uniform storage
	unsigned nVariants;
	TProgramVariant *pVariants;
	unsigned nShaders;
	TProgramShader *pShaders;

	// internal
	u32 *pBlob;
	unsigned nBlobWords;
	unsigned nReceived;		// words stored (the Pico sends them in order)
	u8 *pCode;			// allocation holding all shader code
	boolean bUsed;			// by a draw of the current frame
};

class CPrograms
{
public:
	static const unsigned MaxPrograms = 64;
	static const unsigned MaxBlobWords = 65536;
	static const unsigned MaxUniformWords = 4096;

public:
	CPrograms (void);
	~CPrograms (void);

	void Reset (void);

	// commands; return 0 or a pgpu_error code (*pDetail: more information)
	u32 Create (u32 nId, unsigned nWords);
	u32 Data (u32 nId, unsigned nOffset, const u32 *pWords, unsigned nWords, u32 *pDetail);
	u32 Delete (u32 nId);
	u32 Uniform (u32 nId, unsigned nOffset, const u32 *pWords, unsigned nWords);
	u32 Sampler (u32 nId, unsigned nSampler, unsigned nUnit);

	/// \return nullptr if there is no such complete program
	TProgram *Get (u32 nId);

	/// \return the variant for this primitive class and blend mode, or nullptr
	static const TProgramVariant *FindVariant (const TProgram *pProgram, unsigned nPrim, unsigned nBlend,
						   boolean bTextureTarget);

	/// \brief Mark the program used by the current frame
	void Use (TProgram *pProgram)	{ pProgram->bUsed = TRUE; }

	/// \brief Free code retired during the frame
	void EndFrame (void);

private:
	void Free (TProgram *pProgram, boolean bKeepCode);
	u32 Load (TProgram *pProgram, u32 *pDetail);

private:
	TProgram m_Programs[MaxPrograms + 1];	// ids 1 .. MaxPrograms

	static const unsigned MaxRetired = 64;
	u8 *m_Retired[MaxRetired];
	unsigned m_nRetired;
};

#endif

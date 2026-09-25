//
// v3dperf.cpp
//
// Counter sources per the VideoCore IV 3D Architecture Reference Guide
// (same numbering as the Linux vc4 perfmon).
//
#include "v3dperf.h"
#include <circle/bcm2835.h>
#include <circle/memio.h>
#include <circle/bcmpropertytags.h>
#include <circle/logger.h>

LOGMODULE ("v3dperf");

#define V3D_BASE	(ARM_IO_BASE + 0xC00000)
#define V3D_PCTRC	0x0670
#define V3D_PCTRE	0x0674
#define V3D_PCTR(n)	(0x0680 + (n) * 8)
#define V3D_PCTRS(n)	(0x0684 + (n) * 8)

#define QPUS		12

enum TCounter
{
	FEPValidQuads,
	TLBQuadsFailZ,
	TLBQuadsWritten,
	QPUIdleCycles,
	QPUVertexCycles,
	QPUFragmentCycles,
	QPUInstrCycles,
	QPUStallTMU,
	QPUStallScoreboard,
	QPUStallVaryings,
	QPUICacheHits,
	QPUICacheMisses,
	QPUUniformHits,
	QPUUniformMisses,
	L2CHits,
	L2CMisses
};

static const u8 Sources[CV3DPerf::Counters] =
{
	3,	// FEP valid quads
	5,	// TLB quads with no pixels passing the Z and stencil tests
	9,	// TLB quads with valid pixels written to colour buffer
	13,	// QPU total idle clock cycles for all QPUs
	14,	// QPU total clock cycles for vertex/coordinate shading
	15,	// QPU total clock cycles for fragment shading
	16,	// QPU total clock cycles executing valid instructions
	17,	// QPU total clock cycles stalled waiting for TMUs
	18,	// QPU total clock cycles stalled waiting for scoreboard
	19,	// QPU total clock cycles stalled waiting for varyings
	20,	// QPU instruction cache hits
	21,	// QPU instruction cache misses
	22,	// QPU uniforms cache hits
	23,	// QPU uniforms cache misses
	28,	// L2C total cache hits
	29,	// L2C total cache misses
};

static u32 Read (unsigned nOffset)		{ return read32 (V3D_BASE + nOffset); }
static void Write (unsigned nOffset, u32 nValue)	{ write32 (V3D_BASE + nOffset, nValue); }

CV3DPerf::CV3DPerf (void)
{
}

void CV3DPerf::Start (void)
{
	// keep the V3D powered, the firmware GL power-gates it between jobs
	CBcmPropertyTags Tags;
	TPropertyTagSimple TagQPU;
	TagQPU.nValue = 1;
	Tags.GetTag (0x00030012, &TagQPU, sizeof TagQPU, 4);	// set enable QPU

	u32 nIdent0 = Read (0x0000);
	if ((nIdent0 & 0xFFFFFF) != 0x443356)
	{
		LOGWARN ("V3D registers not accessible (IDENT0 0x%08X)", nIdent0);
	}

	for (unsigned i = 0; i < Counters; i++)
	{
		Write (V3D_PCTRS (i), Sources[i]);
	}

	Write (V3D_PCTRC, 0xFFFF);			// clear
	Write (V3D_PCTRE, (1U << 31) | 0xFFFF);		// enable
}

void CV3DPerf::StopAndLog (const char *pTitle, unsigned nUs, unsigned nPixels)
{
	for (unsigned i = 0; i < Counters; i++)
	{
		m_Values[i] = Read (V3D_PCTR (i));
	}
	Write (V3D_PCTRE, 0);

	const u32 *v = m_Values;
	u64 nTotal = (u64) v[QPUIdleCycles] + v[QPUVertexCycles] + v[QPUFragmentCycles];
	unsigned nMHz = nTotal / QPUS / nUs;

	LOGNOTE ("=== %s: %u us, %u pixels", pTitle, nUs, nPixels);
	LOGNOTE ("V3D clock %u MHz (from QPU cycles), QPU busy %u%%",
		 nMHz, (unsigned) (100 * (nTotal - v[QPUIdleCycles]) / nTotal));
	LOGNOTE ("quads: FEP valid %u, TLB written %u, TLB failed Z %u (%u.%02u quads/pixel)",
		 v[FEPValidQuads], v[TLBQuadsWritten], v[TLBQuadsFailZ],
		 v[FEPValidQuads] / nPixels, (unsigned) (v[FEPValidQuads] * 100ULL / nPixels % 100));
	LOGNOTE ("QPU cycles: fragment %u, vertex %u, executing %u, idle %u",
		 v[QPUFragmentCycles], v[QPUVertexCycles], v[QPUInstrCycles], v[QPUIdleCycles]);
	LOGNOTE ("QPU stalls: TMU %u, scoreboard %u, varyings %u",
		 v[QPUStallTMU], v[QPUStallScoreboard], v[QPUStallVaryings]);
	LOGNOTE ("per FEP quad: %u.%02u executing cycles, %u.%02u fragment cycles",
		 v[QPUInstrCycles] / v[FEPValidQuads], (unsigned) (v[QPUInstrCycles] * 100ULL / v[FEPValidQuads] % 100),
		 v[QPUFragmentCycles] / v[FEPValidQuads], (unsigned) (v[QPUFragmentCycles] * 100ULL / v[FEPValidQuads] % 100));
	LOGNOTE ("caches: instr %u hit / %u miss, uniforms %u hit / %u miss, L2C %u hit / %u miss",
		 v[QPUICacheHits], v[QPUICacheMisses], v[QPUUniformHits], v[QPUUniformMisses],
		 v[L2CHits], v[L2CMisses]);
}

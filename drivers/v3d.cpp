//
// v3d.cpp
//
#include "v3d.h"
#include <circle/bcm2835.h>
#include <circle/bcmpropertytags.h>
#include <circle/memio.h>
#include <circle/synchronize.h>
#include <circle/timer.h>
#include <circle/logger.h>
#include <assert.h>

LOGMODULE ("v3d");

#define V3D_BASE		(ARM_IO_BASE + 0xC00000)

// Registers
#define V3D_IDENT0		0x0000
#define V3D_IDENT1		0x0004
#define V3D_IDENT2		0x0008
#define V3D_L2CACTL		0x0020
#define V3D_SLCACTL		0x0024
#define V3D_INTCTL		0x0030
#define V3D_INTDIS		0x0038
#define V3D_CT0CS		0x0100
#define V3D_CT1CS		0x0104
#define V3D_CT0EA		0x0108
#define V3D_CT1EA		0x010C
#define V3D_CT0CA		0x0110
#define V3D_CT1CA		0x0114
#define V3D_PCS			0x0130
#define V3D_BFC			0x0134
#define V3D_RFC			0x0138
#define V3D_BPOA		0x0308
#define V3D_BPOS		0x030C
#define V3D_ERRSTAT		0x0F20
#define V3D_DBQITC		0x0E2C	// QPU interrupt control (debug)

#define PROPTAG_SET_ENABLE_QPU	0x00030012

// per phase; a long job is legal (dEQP's flush_finish renders for about 1 s),
// both together stay below the devlink watchdog (10 s)
#define JOB_TIMEOUT_US		4000000

void (*CV3D::s_pWaitHandler) (void) = nullptr;

CV3D::CV3D (void)
{
}

CV3D::~CV3D (void)
{
}

boolean CV3D::Initialize (void)
{
	CBcmPropertyTags Tags;
	TPropertyTagSimple TagQPU;
	TagQPU.nValue = 1;
	if (!Tags.GetTag (PROPTAG_SET_ENABLE_QPU, &TagQPU, sizeof TagQPU, 4))
	{
		LOGERR ("Cannot enable QPU");
		return FALSE;
	}

	u32 nIdent0 = Read (V3D_IDENT0);
	if ((nIdent0 & 0xFFFFFF) != 0x443356)		// "V3D"
	{
		LOGERR ("V3D not found (IDENT0 0x%08X)", nIdent0);
		return FALSE;
	}

	u32 nIdent1 = Read (V3D_IDENT1);
	LOGNOTE ("V3D rev %u, %u slices, %u QPUs/slice, %u TMUs/slice, VPM %u KB",
		 nIdent0 >> 24, (nIdent1 >> 4) & 0xF, (nIdent1 >> 8) & 0xF,
		 (nIdent1 >> 12) & 0xF, ((nIdent1 >> 28) & 0xF) ? ((nIdent1 >> 28) & 0xF) : 16);

	Write (V3D_INTDIS, 0xFFFFFFFF);		// we poll

	// reset both control list threads
	Write (V3D_CT0CS, 1 << 15);
	Write (V3D_CT1CS, 1 << 15);

	return TRUE;
}

void *CV3D::Alloc (size_t nSize, size_t nAlign)
{
	u8 *p = new u8[nSize + nAlign];
	assert (p != 0);

	return (void *) (((uintptr) p + nAlign-1) & ~(uintptr) (nAlign-1));
}

u32 CV3D::BusAddress (const void *p)
{
	return BUS_ADDRESS ((uintptr) p);
}

void CV3D::Flush (const void *p, size_t nSize)
{
	CleanAndInvalidateDataCacheRange ((uintptr) p, nSize);
}

boolean CV3D::RunJob (u32 nBinStart, u32 nBinEnd, u32 nRenderStart, u32 nRenderEnd,
		      u32 nOverflowAddress, u32 nOverflowSize,
		      unsigned *pBinUs, unsigned *pRenderUs)
{
	// clear V3D caches (texture/uniform L2C and the slices' caches)
	Write (V3D_L2CACTL, 1 << 2);
	Write (V3D_SLCACTL, 0x0F0F0F0F);

	// reset the flush / frame counters
	Write (V3D_BFC, 1);
	Write (V3D_RFC, 1);

	// binner overflow memory
	Write (V3D_BPOA, nOverflowAddress);
	Write (V3D_BPOS, nOverflowSize);

	unsigned nStart = CTimer::GetClockTicks ();

	Write (V3D_CT0CA, nBinStart);
	Write (V3D_CT0EA, nBinEnd);		// starts binning

	while ((Read (V3D_BFC) & 0xFF) == 0)
	{
		if (s_pWaitHandler)
		{
			(*s_pWaitHandler) ();
		}
		if (CTimer::GetClockTicks () - nStart > JOB_TIMEOUT_US)
		{
			LOGERR ("Binning timeout");
			DumpStatus ();
			Reset ();
			return FALSE;
		}
	}

	unsigned nBinDone = CTimer::GetClockTicks ();

	Write (V3D_CT1CA, nRenderStart);
	Write (V3D_CT1EA, nRenderEnd);		// starts rendering

	while ((Read (V3D_RFC) & 0xFF) == 0)
	{
		if (s_pWaitHandler)
		{
			(*s_pWaitHandler) ();
		}
		if (CTimer::GetClockTicks () - nBinDone > JOB_TIMEOUT_US)
		{
			LOGERR ("Rendering timeout");
			DumpStatus ();
			Reset ();
			return FALSE;
		}
	}

	unsigned nEnd = CTimer::GetClockTicks ();

	if (pBinUs)
	{
		*pBinUs = nBinDone - nStart;
	}

	if (pRenderUs)
	{
		*pRenderUs = nEnd - nBinDone;
	}

	return TRUE;
}

boolean CV3D::RunRender (u32 nRenderStart, u32 nRenderEnd, unsigned *pRenderUs)
{
	Write (V3D_L2CACTL, 1 << 2);		// as RunJob
	Write (V3D_SLCACTL, 0x0F0F0F0F);
	Write (V3D_RFC, 1);

	unsigned nStart = CTimer::GetClockTicks ();

	Write (V3D_CT1CA, nRenderStart);
	Write (V3D_CT1EA, nRenderEnd);		// starts rendering

	while ((Read (V3D_RFC) & 0xFF) == 0)
	{
		if (s_pWaitHandler)
		{
			(*s_pWaitHandler) ();
		}
		if (CTimer::GetClockTicks () - nStart > JOB_TIMEOUT_US)
		{
			LOGERR ("Rendering timeout");
			DumpStatus ();
			Reset ();
			return FALSE;
		}
	}

	if (pRenderUs)
	{
		*pRenderUs = CTimer::GetClockTicks () - nStart;
	}

	return TRUE;
}

void CV3D::DumpStatus (void)
{
	LOGNOTE ("CT0CS 0x%08X CT0CA 0x%08X CT0EA 0x%08X", Read (V3D_CT0CS), Read (V3D_CT0CA), Read (V3D_CT0EA));
	LOGNOTE ("CT1CS 0x%08X CT1CA 0x%08X CT1EA 0x%08X", Read (V3D_CT1CS), Read (V3D_CT1CA), Read (V3D_CT1EA));
	LOGNOTE ("PCS 0x%08X BFC %u RFC %u INTCTL 0x%08X ERRSTAT 0x%08X",
		 Read (V3D_PCS), Read (V3D_BFC), Read (V3D_RFC), Read (V3D_INTCTL), Read (V3D_ERRSTAT));

	// reset the threads for the next job
	Write (V3D_CT0CS, 1 << 15);
	Write (V3D_CT1CS, 1 << 15);
}

// after a job that didn't finish: resetting the control list threads leaves
// the V3D hung (every later job times out), power it off and on
boolean CV3D::Reset (void)
{
	CBcmPropertyTags Tags;
	TPropertyTagSimple TagQPU;
	TagQPU.nValue = 0;
	if (!Tags.GetTag (PROPTAG_SET_ENABLE_QPU, &TagQPU, sizeof TagQPU, 4))
	{
		LOGERR ("Cannot disable QPU");
		return FALSE;
	}
	TagQPU.nValue = 1;
	if (!Tags.GetTag (PROPTAG_SET_ENABLE_QPU, &TagQPU, sizeof TagQPU, 4))
	{
		LOGERR ("Cannot enable QPU");
		return FALSE;
	}

	Write (V3D_INTDIS, 0xFFFFFFFF);
	Write (V3D_INTCTL, 0xFFFFFFFF);		// clear what is pending
	Write (V3D_CT0CS, 1 << 15);
	Write (V3D_CT1CS, 1 << 15);
	LOGNOTE ("Reset (IDENT0 0x%08X)", Read (V3D_IDENT0));

	return TRUE;
}

u32 CV3D::Read (unsigned nOffset)
{
	return read32 (V3D_BASE + nOffset);
}

void CV3D::Write (unsigned nOffset, u32 nValue)
{
	write32 (V3D_BASE + nOffset, nValue);
}

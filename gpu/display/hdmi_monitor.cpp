//
// hdmi_monitor.cpp
//
#include "hdmi_monitor.h"
#include <circle/bcm2835.h>
#include <circle/machineinfo.h>
#include <circle/memio.h>
#include <circle/timer.h>
#include <circle/logger.h>
#include <circle/util.h>

#define HPD_PIN			46		// low: connected (Pi Zero)

#define SAMPLE_US		20000		// hot-plug line
#define STABLE_SAMPLES		10		// a change counts after 200 ms
#define EDID_TRIES		3		// a monitor may answer late after plugging in
#define EDID_RETRY_MS		100
#define EDID_LATER_US		1000000		// then tried again every second
#define EDID_SETTLE_US		2000000		// after a hot plug: the firmware reads it
						// (the same BSC2) and sets HDMI up first

// BSC2 (the HDMI DDC bus)
#define BSC2_BASE		(ARM_IO_BASE + 0x805000)
#define BSC_C			(BSC2_BASE + 0x00)
	#define BSC_C_I2CEN		(1 << 15)
	#define BSC_C_ST		(1 << 7)
	#define BSC_C_CLEAR		(1 << 4)
	#define BSC_C_READ		(1 << 0)
#define BSC_S			(BSC2_BASE + 0x04)
	#define BSC_S_CLKT		(1 << 9)
	#define BSC_S_ERR		(1 << 8)
	#define BSC_S_RXD		(1 << 5)
	#define BSC_S_DONE		(1 << 1)
#define BSC_DLEN		(BSC2_BASE + 0x08)
#define BSC_A			(BSC2_BASE + 0x0C)
#define BSC_FIFO		(BSC2_BASE + 0x10)
#define BSC_DIV			(BSC2_BASE + 0x14)
#define DDC_ADDRESS		0x50
#define DDC_CLOCK		100000

// the HDMI pixel valve (as in Linux vc4_regs.h)
#define PV2_BASE		(ARM_IO_BASE + 0x807000)
#define PV_HORZB		(PV2_BASE + 0x10)	// HFP << 16 | HACTIVE
#define PV_VERTB		(PV2_BASE + 0x18)	// VFP << 16 | VACTIVE

LOGMODULE ("hdmi");

CHDMIMonitor::CHDMIMonitor (void)
:	m_bLastSample (FALSE),
	m_nSameSamples (0),
	m_nLastSampleTicks (0),
	m_nLastEDIDTicks (0)
{
	memset (&m_State, 0, sizeof m_State);
}

void CHDMIMonitor::Initialize (void)
{
	m_bLastSample = ReadHPD ();
	m_nSameSamples = STABLE_SAMPLES;
	m_nLastSampleTicks = CTimer::GetClockTicks ();
	if (m_bLastSample)
	{
		Connected (TRUE);
	}
	else
	{
		LOGNOTE ("No monitor");
	}
	ReadSignal ();
}

boolean CHDMIMonitor::Update (void)
{
	unsigned nTicks = CTimer::GetClockTicks ();
	if (nTicks - m_nLastSampleTicks < SAMPLE_US)
	{
		return FALSE;
	}
	m_nLastSampleTicks = nTicks;

	// connected, but the EDID hasn't come: once a second (signed: after a hot
	// plug the last try is set in the future, see Connected)
	if (   m_State.bConnected && !m_State.bEDID
	    && (int) (nTicks - m_nLastEDIDTicks) >= EDID_LATER_US)
	{
		m_nLastEDIDTicks = nTicks;
		u8 Block[128];
		if (ReadEDID (Block) && ParseEDID (Block))
		{
			return TRUE;
		}
	}

	boolean bSample = ReadHPD ();
	if (bSample != m_bLastSample)
	{
		m_bLastSample = bSample;
		m_nSameSamples = 1;
		return FALSE;
	}
	if (   m_nSameSamples >= STABLE_SAMPLES
	    || ++m_nSameSamples < STABLE_SAMPLES
	    || bSample == m_State.bConnected)
	{
		return FALSE;
	}

	if (bSample)
	{
		Connected (FALSE);
	}
	else
	{
		LOGNOTE ("Monitor disconnected");
		memset (&m_State, 0, sizeof m_State);
	}
	ReadSignal ();

	return TRUE;
}

boolean CHDMIMonitor::ReadHPD (void)
{
	return !(read32 (ARM_GPIO_GPLEV0 + HPD_PIN / 32 * 4) & 1 << (HPD_PIN % 32));
}

// a monitor has appeared: its EDID now (at boot: the firmware is done with
// HDMI) or later (a hot plug: from EDID_SETTLE_US on, every second, see Update)
void CHDMIMonitor::Connected (boolean bNow)
{
	memset (&m_State, 0, sizeof m_State);
	m_State.bConnected = TRUE;

	if (bNow)
	{
		u8 Block[128];
		for (unsigned i = 0; i < EDID_TRIES; i++)
		{
			if (i)
			{
				CTimer::SimpleMsDelay (EDID_RETRY_MS);
			}
			if (ReadEDID (Block) && ParseEDID (Block))
			{
				return;
			}
		}
	}

	LOGNOTE ("Monitor connected, EDID later");
	m_nLastEDIDTicks = CTimer::GetClockTicks () - EDID_LATER_US + EDID_SETTLE_US;
}

// the monitor's preferred mode (the first detailed timing) and name from EDID
// block 0; FALSE if the block isn't valid
boolean CHDMIMonitor::ParseEDID (const u8 *pBlock)
{
	static const u8 Header[8] = {0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0};
	u8 uchSum = 0;
	for (unsigned i = 0; i < 128; i++)
	{
		uchSum += pBlock[i];
	}
	const u8 *d = pBlock + 54;
	unsigned nClockKHz = (d[0] | d[1] << 8) * 10;
	if (memcmp (pBlock, Header, sizeof Header) != 0 || uchSum != 0 || nClockKHz == 0)
	{
		LOGWARN ("Invalid EDID (header %02X %02X, checksum %02X)", pBlock[0], pBlock[1], uchSum);
		return FALSE;
	}

	m_State.bEDID = TRUE;
	m_State.nWidth = d[2] | (d[4] & 0xF0) << 4;
	m_State.nHeight = d[5] | (d[7] & 0xF0) << 4;
	unsigned nHTotal = m_State.nWidth + (d[3] | (d[4] & 0x0F) << 8);
	unsigned nVTotal = m_State.nHeight + (d[6] | (d[7] & 0x0F) << 8);
	m_State.nRefreshMilliHz = (unsigned) ((u64) nClockKHz * 1000000 / (nHTotal * nVTotal));

	memset (m_State.Name, 0, sizeof m_State.Name);
	for (unsigned n = 0; n < 4; n++)		// the name: display descriptor FC
	{
		const u8 *p = pBlock + 54 + 18 * n;
		if (p[0] == 0 && p[1] == 0 && p[3] == 0xFC)
		{
			for (unsigned i = 0; i < 13 && p[5 + i] != '\n' && p[5 + i] != 0; i++)
			{
				m_State.Name[i] = p[5 + i] >= ' ' && p[5 + i] < 0x7F ? p[5 + i] : '?';
			}
		}
	}

	LOGNOTE ("Monitor \"%s\": %ux%u at %u.%03u Hz", m_State.Name, m_State.nWidth,
		 m_State.nHeight, m_State.nRefreshMilliHz / 1000, m_State.nRefreshMilliHz % 1000);

	return TRUE;
}

// EDID block 0 over DDC at 100 kHz (about 12 ms)
boolean CHDMIMonitor::ReadEDID (u8 *pBlock)
{
	u32 nDiv = read32 (BSC_DIV);		// the firmware's, restored afterwards
	write32 (BSC_DIV, CMachineInfo::Get ()->GetClockRate (CLOCK_ID_CORE) / DDC_CLOCK);
	write32 (BSC_S, BSC_S_CLKT | BSC_S_ERR | BSC_S_DONE);
	write32 (BSC_A, DDC_ADDRESS);

	// the offset (0), then 128 bytes
	write32 (BSC_DLEN, 1);
	write32 (BSC_C, BSC_C_I2CEN | BSC_C_CLEAR);
	write32 (BSC_FIFO, 0);
	write32 (BSC_C, BSC_C_I2CEN | BSC_C_ST);
	unsigned nTimeout = 10000;
	while (!(read32 (BSC_S) & BSC_S_DONE) && --nTimeout)
	{
		CTimer::SimpleusDelay (1);
	}

	unsigned n = 0;
	if (nTimeout && !(read32 (BSC_S) & (BSC_S_ERR | BSC_S_CLKT)))
	{
		write32 (BSC_S, BSC_S_CLKT | BSC_S_ERR | BSC_S_DONE);
		write32 (BSC_DLEN, 128);
		write32 (BSC_C, BSC_C_I2CEN | BSC_C_ST | BSC_C_CLEAR | BSC_C_READ);
		nTimeout = 50000;
		while (n < 128 && --nTimeout)
		{
			u32 nStatus = read32 (BSC_S);
			if (nStatus & BSC_S_RXD)
			{
				pBlock[n++] = read32 (BSC_FIFO);
			}
			else if (nStatus & (BSC_S_ERR | BSC_S_CLKT))
			{
				break;
			}
			else
			{
				CTimer::SimpleusDelay (1);
			}
		}
	}

	write32 (BSC_S, BSC_S_CLKT | BSC_S_ERR | BSC_S_DONE);
	write32 (BSC_C, BSC_C_I2CEN | BSC_C_CLEAR);
	write32 (BSC_DIV, nDiv);

	return n == 128;
}

void CHDMIMonitor::ReadSignal (void)
{
	m_State.nSignalWidth = read32 (PV_HORZB) & 0xFFFF;
	m_State.nSignalHeight = read32 (PV_VERTB) & 0xFFFF;
}

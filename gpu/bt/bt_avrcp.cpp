//
// bt_avrcp.cpp
//
#include "bt_avrcp.h"
#include <circle/logger.h>
#include <circle/string.h>
#include <circle/timer.h>
#include <circle/util.h>

LOGMODULE ("btavrcp");

// AVCTP: a transaction's label (bits 7-4), a single packet, command or
// response (bit 1), then the profile: AVRCP's
#define AVCTP_RESPONSE		0x02
#define AVCTP_BAD_PROFILE	0x01
#define AVCTP_HEADER		3

// AV/C: the frame's type, the subunit (the panel; the unit itself for the two
// questions about it), the operation
#define CTYPE_CONTROL		0x0
#define CTYPE_STATUS		0x1
#define CTYPE_NOTIFY		0x3
#define RSP_NOT_IMPLEMENTED	0x8
#define RSP_ACCEPTED		0x9
#define RSP_REJECTED		0xA
#define RSP_STABLE		0xC
#define RSP_CHANGED		0xD
#define RSP_INTERIM		0xF
#define SUBUNIT_PANEL		0x48
#define SUBUNIT_UNIT		0xFF
#define OP_VENDOR		0x00
#define OP_UNIT_INFO		0x30
#define OP_SUBUNIT_INFO		0x31
#define OP_PASS_THROUGH		0x7C

// AVRCP's own commands (vendor dependent, the Bluetooth SIG's: 00 19 58)
#define PDU_GET_CAPABILITIES	0x10
#define PDU_REGISTER		0x31
#define PDU_SET_VOLUME		0x50
#define CAP_COMPANY		0x02
#define CAP_EVENTS		0x03
#define EVENT_VOLUME		0x0D
#define ERR_INVALID_COMMAND	0x00
#define ERR_INVALID_PARAMETER	0x01

#define PASS_VOLUME_UP		0x41
#define PASS_VOLUME_DOWN	0x42

#define NONE			(~0U)
#define START_US		300000		// after the channel opens: the speaker's own questions first
#define ANSWER_US		2000000		// a volume not answered: given up
#define ECHO_US			500000		// a change it reports this soon after ours is ours

CBTAVRCP::CBTAVRCP (CBTAVRCPHost *pHost)
:	m_pHost (pHost),
	m_bOpen (FALSE),
	m_bVolume (FALSE),
	m_bAsked (FALSE),
	m_nLabel (0),
	m_nSpeaker (NONE),
	m_nWanted (NONE),
	m_nSent (NONE),
	m_bSetting (FALSE),
	m_nSetAt (0),
	m_bChanged (FALSE),
	m_nButtons (0),
	m_nOpenAt (0),
	m_bTrace (FALSE)
{
}

void CBTAVRCP::ChannelOpen (void)
{
	LOGNOTE ("The remote control channel is open");
	m_bOpen = TRUE;
	m_bVolume = m_bAsked = m_bSetting = m_bChanged = FALSE;
	m_nSpeaker = m_nSent = NONE;
	m_nButtons = 0;
	m_nOpenAt = CTimer::GetClockTicks ();
}

void CBTAVRCP::ChannelClosed (void)
{
	m_bOpen = m_bVolume = m_bSetting = m_bChanged = FALSE;
	m_nSpeaker = m_nWanted = NONE;
	m_nButtons = 0;
}

void CBTAVRCP::SetVolume (unsigned nVolume)
{
	m_nWanted = nVolume > MaxVolume ? MaxVolume : nVolume;
}

boolean CBTAVRCP::VolumeChanged (unsigned *pVolume)
{
	if (!m_bChanged)
	{
		return FALSE;
	}
	m_bChanged = FALSE;
	*pVolume = m_nSpeaker;
	return TRUE;
}

int CBTAVRCP::GetButtons (void)
{
	int n = m_nButtons;
	m_nButtons = 0;
	return n;
}

static void Trace (const char *pWhat, const u8 *p, unsigned n)
{
	CString Line, Byte;
	for (unsigned i = 0; i < n && i < 40; i++)
	{
		Byte.Format (" %02X", p[i]);
		Line.Append (Byte);
	}
	LOGNOTE ("%s:%s", pWhat, (const char *) Line);
}

void CBTAVRCP::Probe (u8 nType, u8 nPDU, const u8 *pParams, unsigned nBytes)
{
	m_bTrace = TRUE;
	if (m_bOpen && nBytes <= 8)
	{
		Vendor (nType, nPDU, pParams, nBytes);
	}
	else
	{
		LOGNOTE ("No remote control channel");
	}
}

// one of AVRCP's commands
void CBTAVRCP::Vendor (u8 nType, u8 nPDU, const u8 *pParams, unsigned nBytes)
{
	u8 Frame[AVCTP_HEADER + 10 + 8];
	Frame[0] = (u8) (m_nLabel++ << 4);
	Frame[1] = 0x11;
	Frame[2] = 0x0E;
	u8 *p = Frame + AVCTP_HEADER;
	p[0] = nType;
	p[1] = SUBUNIT_PANEL;
	p[2] = OP_VENDOR;
	p[3] = 0x00;
	p[4] = 0x19;
	p[5] = 0x58;
	p[6] = nPDU;
	p[7] = 0;
	p[8] = 0;
	p[9] = (u8) nBytes;
	memcpy (p + 10, pParams, nBytes);
	if (m_bTrace)
	{
		Trace ("Sent", Frame, AVCTP_HEADER + 10 + nBytes);
	}
	m_pHost->SendControl (Frame, AVCTP_HEADER + 10 + nBytes);
}

// tell us when its volume changes (answered at once with the volume it has,
// then once when it changes: asked again each time)
void CBTAVRCP::Register (void)
{
	static const u8 Params[5] = {EVENT_VOLUME, 0, 0, 0, 0};
	Vendor (CTYPE_NOTIFY, PDU_REGISTER, Params, sizeof Params);
}

void CBTAVRCP::Update (void)
{
	if (!m_bOpen)
	{
		return;
	}
	unsigned nNow = CTimer::GetClockTicks ();
	if (!m_bAsked)
	{
		if (nNow - m_nOpenAt >= START_US)
		{
			m_bAsked = TRUE;
			Register ();
		}
		return;
	}
	if (m_bSetting && nNow - m_nSetAt > ANSWER_US)
	{
		m_bSetting = FALSE;
	}
	if (m_bVolume && !m_bSetting && m_nWanted != NONE && m_nWanted != m_nSent)
	{
		u8 nVolume = (u8) m_nWanted;
		Vendor (CTYPE_CONTROL, PDU_SET_VOLUME, &nVolume, 1);
		m_nSent = m_nWanted;
		m_bSetting = TRUE;
		m_nSetAt = nNow;
	}
}

void CBTAVRCP::OnData (const u8 *pData, unsigned nBytes)
{
	if (m_bTrace)
	{
		Trace ("Received", pData, nBytes);
	}
	if (nBytes < AVCTP_HEADER + 3 || (pData[0] & 0x0C))	// (a part of a longer one: none of ours is)
	{
		return;
	}
	if (pData[1] != 0x11 || pData[2] != 0x0E)		// another profile's: not here
	{
		if (!(pData[0] & AVCTP_RESPONSE))
		{
			u8 Answer[AVCTP_HEADER] = {(u8) (pData[0] | AVCTP_RESPONSE | AVCTP_BAD_PROFILE), pData[1], pData[2]};
			m_pHost->SendControl (Answer, sizeof Answer);
		}
		return;
	}
	if (pData[0] & AVCTP_RESPONSE)
	{
		OnAnswer (pData, nBytes);
	}
	else
	{
		OnCommand (pData, nBytes);
	}
}

// our answer to a command: its frame with the answer's type, and for one of
// AVRCP's commands these parameters
void CBTAVRCP::Answer (const u8 *pFrame, unsigned nBytes, u8 nType, const u8 *pParams, unsigned nParams)
{
	u8 Out[64];
	if (pParams)
	{
		nBytes = AVCTP_HEADER + 10;		// (up to its own parameters)
	}
	else if (nBytes > sizeof Out)
	{
		nBytes = sizeof Out;
	}
	memcpy (Out, pFrame, nBytes);
	Out[0] |= AVCTP_RESPONSE;
	Out[AVCTP_HEADER] = nType;
	if (pParams)
	{
		Out[AVCTP_HEADER + 7] = 0;
		Out[AVCTP_HEADER + 8] = 0;
		Out[AVCTP_HEADER + 9] = (u8) nParams;
		memcpy (Out + nBytes, pParams, nParams);
		nBytes += nParams;
	}
	m_pHost->SendControl (Out, nBytes);
}

// the speaker's commands to a player
void CBTAVRCP::OnCommand (const u8 *pFrame, unsigned nBytes)
{
	const u8 *p = pFrame + AVCTP_HEADER;
	unsigned n = nBytes - AVCTP_HEADER;
	switch (p[2])
	{
	case OP_UNIT_INFO:				// what are you: a panel, the SIG's
	{
		u8 Out[AVCTP_HEADER + 9] = {(u8) (pFrame[0] | AVCTP_RESPONSE), 0x11, 0x0E,
					    RSP_STABLE, SUBUNIT_UNIT, OP_UNIT_INFO, 0x07, SUBUNIT_PANEL, 0xFF, 0x00, 0x19, 0x58};
		m_pHost->SendControl (Out, sizeof Out);
		break;
	}

	case OP_SUBUNIT_INFO:
	{
		u8 Out[AVCTP_HEADER + 8] = {(u8) (pFrame[0] | AVCTP_RESPONSE), 0x11, 0x0E,
					    RSP_STABLE, SUBUNIT_UNIT, OP_SUBUNIT_INFO, 0x07, SUBUNIT_PANEL, 0xFF, 0xFF, 0xFF};
		m_pHost->SendControl (Out, sizeof Out);
		break;
	}

	case OP_PASS_THROUGH:				// a button: taken (its press and its release)
		if (n >= 4 && !(p[3] & 0x80))
		{
			u8 nButton = p[3] & 0x7F;
			m_nButtons += nButton == PASS_VOLUME_UP ? 1 : nButton == PASS_VOLUME_DOWN ? -1 : 0;
			LOGNOTE ("The speaker's button %02X", nButton);
		}
		Answer (pFrame, nBytes, RSP_ACCEPTED, nullptr, 0);
		break;

	case OP_VENDOR:
		if (n >= 10 && p[3] == 0x00 && p[4] == 0x19 && p[5] == 0x58)
		{
			if (p[6] == PDU_GET_CAPABILITIES && n >= 11 && p[10] == CAP_COMPANY)
			{
				static const u8 Company[5] = {CAP_COMPANY, 1, 0x00, 0x19, 0x58};
				Answer (pFrame, nBytes, RSP_STABLE, Company, sizeof Company);
			}
			else if (p[6] == PDU_GET_CAPABILITIES && n >= 11 && p[10] == CAP_EVENTS)
			{
				static const u8 Events[2] = {CAP_EVENTS, 0};	// (none: nothing here to follow)
				Answer (pFrame, nBytes, RSP_STABLE, Events, sizeof Events);
			}
			else
			{
				u8 nError = p[6] == PDU_REGISTER || p[6] == PDU_GET_CAPABILITIES ? ERR_INVALID_PARAMETER
											       : ERR_INVALID_COMMAND;
				Answer (pFrame, nBytes, RSP_REJECTED, &nError, 1);
			}
			break;
		}
		// (another vendor's)

	default:
		Answer (pFrame, nBytes, RSP_NOT_IMPLEMENTED, nullptr, 0);
		break;
	}
}

// its answers to ours
void CBTAVRCP::OnAnswer (const u8 *pFrame, unsigned nBytes)
{
	const u8 *p = pFrame + AVCTP_HEADER;
	unsigned n = nBytes - AVCTP_HEADER;
	if (p[2] != OP_VENDOR || n < 10)
	{
		return;
	}
	u8 nType = p[0] & 0x0F, nPDU = p[6];
	unsigned nParams = p[8] << 8 | p[9];
	const u8 *q = p + 10;
	if (nParams > n - 10)
	{
		nParams = n - 10;
	}
	if (nPDU == PDU_REGISTER)
	{
		if ((nType == RSP_INTERIM || nType == RSP_CHANGED) && nParams >= 2 && q[0] == EVENT_VOLUME)
		{
			unsigned nVolume = q[1] & 0x7F;
			if (!m_bVolume)
			{
				m_bVolume = TRUE;
				LOGNOTE ("The speaker's volume is set from here (it has %u of 127)", nVolume);
			}
			if (nType == RSP_CHANGED)
			{
				// its buttons (what it reports just after we set it is our own change)
				if (nVolume != m_nSpeaker && !m_bSetting && CTimer::GetClockTicks () - m_nSetAt > ECHO_US)
				{
					LOGNOTE ("The speaker's volume: %u of 127 (its buttons)", nVolume);
					m_bChanged = TRUE;
					m_nWanted = m_nSent = nVolume;
				}
				m_nSpeaker = nVolume;
				Register ();
			}
			else
			{
				m_nSpeaker = nVolume;
			}
		}
		else if (nType == RSP_REJECTED || nType == RSP_NOT_IMPLEMENTED)
		{
			LOGNOTE ("The speaker keeps its volume to itself (%X %02X)", nType, nParams ? q[0] : 0);
		}
	}
	else if (nPDU == PDU_SET_VOLUME)
	{
		m_bSetting = FALSE;
		if (nType == RSP_ACCEPTED && nParams >= 1)
		{
			m_nSpeaker = q[0] & 0x7F;	// (what it made of it: its steps may be coarser)
		}
		else
		{
			LOGNOTE ("The speaker didn't take the volume (%X)", nType);
		}
	}
}

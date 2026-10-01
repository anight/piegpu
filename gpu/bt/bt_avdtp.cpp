//
// bt_avdtp.cpp
//
#include "bt_avdtp.h"
#include <circle/logger.h>
#include <circle/timer.h>
#include <circle/util.h>

LOGMODULE ("btavdtp");

#define OUR_SEID		1
#define WAIT_US			400000		// for the speaker's own commands, before ours
#define ANSWER_TIMEOUT_US	6000000
#define MAX_BITPOOL		53		// (A2DP's high quality at 44.1 kHz joint stereo)

#define SIG_DISCOVER		0x01
#define SIG_GET_CAPABILITIES	0x02
#define SIG_SET_CONFIGURATION	0x03
#define SIG_GET_CONFIGURATION	0x04
#define SIG_RECONFIGURE		0x05
#define SIG_OPEN		0x06
#define SIG_START		0x07
#define SIG_CLOSE		0x08
#define SIG_SUSPEND		0x09
#define SIG_ABORT		0x0A
#define SIG_SECURITY_CONTROL	0x0B
#define SIG_GET_ALL_CAPS	0x0C
#define SIG_DELAY_REPORT	0x0D

#define MSG_COMMAND		0
#define MSG_GENERAL_REJECT	1
#define MSG_ACCEPT		2
#define MSG_REJECT		3

#define CAT_MEDIA_TRANSPORT	0x01
#define CAT_MEDIA_CODEC		0x07

#define ERR_BAD_STATE		0x31
#define ERR_SEP_IN_USE		0x13
#define ERR_BAD_ACP_SEID	0x12
#define ERR_UNSUPPORTED_CONFIG	0x29
#define ERR_NOT_SUPPORTED	0x19

CBTAVDTP::CBTAVDTP (CBTAVDTPHost *pHost)
:	m_pHost (pHost),
	m_State (Idle),
	m_nSince (0),
	m_nLabel (0),
	m_nSignal (0),
	m_nEndPoints (0),
	m_nEndPoint (0),
	m_nTheirSEID (0),
	m_bTheirsKnown (FALSE),
	m_bMedia (FALSE),
	m_nRate (44100),
	m_bWanted (FALSE)
{
	memset (&m_Config, 0, sizeof m_Config);
}

void CBTAVDTP::Enter (TState State)
{
	m_State = State;
	m_nSince = CTimer::GetClockTicks ();
}

void CBTAVDTP::Fail (const char *pWhy)
{
	LOGWARN ("No stream: %s", pWhy);
	m_nSignal = 0;
	Enter (Failed);
	m_pHost->OnStream ();
}

void CBTAVDTP::SignallingOpen (void)
{
	m_nSignal = 0;
	m_nEndPoints = m_nEndPoint = 0;
	m_bTheirsKnown = FALSE;
	m_bMedia = FALSE;
	Enter (Waiting);
}

void CBTAVDTP::SignallingClosed (void)
{
	m_nSignal = 0;
	m_bMedia = FALSE;
	Enter (Idle);
	m_pHost->OnStream ();
}

void CBTAVDTP::MediaOpen (void)
{
	m_bMedia = TRUE;
	if (m_State == WaitMedia)
	{
		LOGNOTE ("The stream is open: SBC %u Hz, %s, %u blocks, %u subbands, %s, bitpool %u", m_Config.nRate,
			 m_Config.nChannels == 1 ? "mono" : m_Config.bJoint ? "joint stereo" : m_Config.bDual ? "dual channel" : "stereo",
			 m_Config.nBlocks, m_Config.nSubbands, m_Config.bSNR ? "SNR" : "loudness", m_Config.nBitpool);
		Enter (Open);
		m_pHost->OnStream ();
		Go ();
	}
}

void CBTAVDTP::MediaClosed (void)
{
	m_bMedia = FALSE;
	if (m_State >= WaitMedia && m_State <= Reconfiguring)
	{
		m_nSignal = 0;
		Enter (Failed);
		m_pHost->OnStream ();
	}
}

void CBTAVDTP::SetRate (unsigned nRate)
{
	m_nRate = nRate == 48000 ? 48000 : 44100;
	Go ();
}

void CBTAVDTP::SetWanted (boolean bWanted)
{
	m_bWanted = bWanted;
	Go ();
}

// what's wanted, from where the stream is: another rate first (suspended for
// it), then started or suspended
void CBTAVDTP::Go (void)
{
	if (m_nSignal)					// (an answer is due first)
	{
		return;
	}
	const u8 SEID = (u8) (m_nTheirSEID << 2);
	boolean bRate = m_bWanted && m_bTheirsKnown && m_nRate != m_Config.nRate
			&& Choose (m_TheirCaps, m_nRate, &m_NewConfig, m_NewBytes) && m_NewConfig.nRate == m_nRate;
	if (m_State == Streaming && (!m_bWanted || bRate))
	{
		Enter (Suspending);
		Command (SIG_SUSPEND, &SEID, 1);
	}
	else if (m_State == Open && bRate)
	{
		Configure (TRUE);
	}
	else if (m_State == Open && m_bWanted)
	{
		Enter (Starting);
		Command (SIG_START, &SEID, 1);
	}
}

void CBTAVDTP::Command (u8 nSignal, const u8 *pData, unsigned nBytes)
{
	u8 Message[2 + 32];
	m_nLabel = (m_nLabel + 1) & 15;
	m_nSignal = nSignal;
	Message[0] = (u8) (m_nLabel << 4 | MSG_COMMAND);
	Message[1] = nSignal;
	if (nBytes)
	{
		memcpy (Message + 2, pData, nBytes);
	}
	m_nSince = CTimer::GetClockTicks ();
	m_pHost->SendSignal (Message, 2 + nBytes);
}

void CBTAVDTP::Answer (u8 nLabel, u8 nSignal, boolean bAccept, const u8 *pData, unsigned nBytes)
{
	u8 Message[2 + 32];
	Message[0] = (u8) (nLabel << 4 | (bAccept ? MSG_ACCEPT : MSG_REJECT));
	Message[1] = nSignal;
	if (nBytes)
	{
		memcpy (Message + 2, pData, nBytes);
	}
	m_pHost->SendSignal (Message, 2 + nBytes);
}

// the SBC codec's four bytes among a list of capabilities (each: its category, its length, its data)
const u8 *CBTAVDTP::FindSBC (const u8 *p, unsigned n)
{
	for (unsigned i = 0; i + 2 <= n; i += 2 + p[i + 1])
	{
		if (p[i] == CAT_MEDIA_CODEC && p[i + 1] >= 6 && i + 8 <= n && (p[i + 2] >> 4) == 0 && p[i + 3] == 0)
		{
			return p + i + 4;		// (audio, SBC)
		}
	}
	return nullptr;
}

// A configuration out of what they can do, at this rate if they have it
// (else at the other of 44.1 and 48 kHz): one bit of each kind
boolean CBTAVDTP::Choose (const u8 *pCaps, unsigned nRate, TConfig *pConfig, u8 *pBytes) const
{
	u8 nRateBit = nRate == 48000 ? 0x10 : 0x20;
	if (!(pCaps[0] & nRateBit))
	{
		nRateBit ^= 0x30;
	}
	u8 nMode = pCaps[0] & 0x01 ? 0x01 : pCaps[0] & 0x02 ? 0x02 : pCaps[0] & 0x04 ? 0x04 : 0x08;
	u8 nBlocks = pCaps[1] & 0x10 ? 0x10 : pCaps[1] & 0x20 ? 0x20 : pCaps[1] & 0x40 ? 0x40 : 0x80;
	u8 nSubbands = pCaps[1] & 0x04 ? 0x04 : 0x08;
	u8 nAllocation = pCaps[1] & 0x01 ? 0x01 : 0x02;
	if (!(pCaps[0] & nRateBit) || !(pCaps[0] & nMode) || !(pCaps[1] & nBlocks) || !(pCaps[1] & nSubbands)
	    || !(pCaps[1] & nAllocation))
	{
		return FALSE;
	}
	unsigned nMax = nRateBit == 0x10 ? MAX_BITPOOL - 2 : MAX_BITPOOL;	// (the same bit rate at 48 kHz)
	pBytes[0] = nRateBit | nMode;
	pBytes[1] = nBlocks | nSubbands | nAllocation;
	pBytes[2] = pCaps[2] < 2 ? 2 : pCaps[2];
	pBytes[3] = pCaps[3] < nMax ? pCaps[3] : (u8) nMax;
	if (pBytes[3] < pBytes[2])
	{
		pBytes[3] = pBytes[2];
	}
	return Parse (pBytes, pConfig);
}

// a configuration's four bytes: one bit of each kind set
boolean CBTAVDTP::Parse (const u8 *p, TConfig *pConfig)
{
	u8 nRate = p[0] & 0xF0, nMode = p[0] & 0x0F, nBlocks = p[1] & 0xF0, nSubbands = p[1] & 0x0C, nAllocation = p[1] & 0x03;
	if (   (nRate != 0x20 && nRate != 0x10)
	    || (nMode != 1 && nMode != 2 && nMode != 4 && nMode != 8)
	    || (nBlocks != 0x10 && nBlocks != 0x20 && nBlocks != 0x40 && nBlocks != 0x80)
	    || (nSubbands != 4 && nSubbands != 8) || (nAllocation != 1 && nAllocation != 2))
	{
		return FALSE;
	}
	pConfig->nRate = nRate == 0x10 ? 48000 : 44100;
	pConfig->nChannels = nMode == 8 ? 1 : 2;
	pConfig->bJoint = nMode == 1;
	pConfig->bDual = nMode == 4;
	pConfig->nBlocks = nBlocks == 0x10 ? 16 : nBlocks == 0x20 ? 12 : nBlocks == 0x40 ? 8 : 4;
	pConfig->nSubbands = nSubbands == 4 ? 8 : 4;
	pConfig->bSNR = nAllocation == 2;
	pConfig->nBitpool = p[3] < MAX_BITPOOL ? p[3] : MAX_BITPOOL;
	return TRUE;
}

void CBTAVDTP::NextEndPoint (void)
{
	if (m_nEndPoint >= m_nEndPoints)
	{
		Fail ("the device has no free SBC audio sink");
		return;
	}
	const u8 SEID = (u8) (m_EndPoint[m_nEndPoint] << 2);
	Enter (GettingCaps);
	Command (SIG_GET_CAPABILITIES, &SEID, 1);
}

// our configuration to their end point: the first (bAgain FALSE), or another rate
void CBTAVDTP::Configure (boolean bAgain)
{
	u8 Data[12];
	unsigned n = 0;
	Data[n++] = (u8) (m_nTheirSEID << 2);
	if (!bAgain)
	{
		Data[n++] = OUR_SEID << 2;
		Data[n++] = CAT_MEDIA_TRANSPORT;
		Data[n++] = 0;
	}
	Data[n++] = CAT_MEDIA_CODEC;
	Data[n++] = 6;
	Data[n++] = 0;					// audio
	Data[n++] = 0;					// SBC
	memcpy (Data + n, m_NewBytes, 4);
	n += 4;
	Enter (bAgain ? Reconfiguring : Configuring);
	Command (bAgain ? SIG_RECONFIGURE : SIG_SET_CONFIGURATION, Data, n);
}

void CBTAVDTP::OnSignal (const u8 *p, unsigned n)
{
	if (n < 2)
	{
		return;
	}
	u8 nLabel = p[0] >> 4, nPacket = p[0] >> 2 & 3, nType = p[0] & 3, nSignal = p[1] & 0x3F;
	if (nPacket != 0)				// (a message in pieces: none of ours needs them)
	{
		LOGWARN ("A fragmented message (signal %02X) passed over", nSignal);
		return;
	}
	if (nType == MSG_COMMAND)
	{
		OnCommand (nLabel, nSignal, p + 2, n - 2);
	}
	else if (m_nSignal && nLabel == m_nLabel && (nSignal == m_nSignal || nType == MSG_GENERAL_REJECT))
	{
		u8 nAnswered = m_nSignal;
		m_nSignal = 0;
		OnAnswer (nAnswered, nType == MSG_ACCEPT, p + 2, n - 2);
	}
}

void CBTAVDTP::OnAnswer (u8 nSignal, boolean bAccept, const u8 *p, unsigned n)
{
	if (!bAccept)
	{
		LOGNOTE ("Signal %02X rejected (%02X %02X)", nSignal, n > 0 ? p[0] : 0, n > 1 ? p[1] : 0);
	}
	switch (nSignal)
	{
	case SIG_DISCOVER:				// their end points: an ID and what it is, two bytes each
		if (!bAccept)
		{
			Fail ("the device doesn't say what it has");
			break;
		}
		m_nEndPoints = m_nEndPoint = 0;
		for (unsigned i = 0; i + 2 <= n && m_nEndPoints < MaxEndPoints; i += 2)
		{
			boolean bInUse = !!(p[i] & 2), bSink = !!(p[i + 1] & 8);
			if (!bInUse && bSink && (p[i + 1] >> 4) == 0)		// (audio)
			{
				m_EndPoint[m_nEndPoints++] = p[i] >> 2;
			}
		}
		NextEndPoint ();
		break;

	case SIG_GET_CAPABILITIES:
	{
		const u8 *pSBC = bAccept ? FindSBC (p, n) : nullptr;
		if (pSBC && Choose (pSBC, m_nRate, &m_NewConfig, m_NewBytes))
		{
			m_nTheirSEID = m_EndPoint[m_nEndPoint];
			memcpy (m_TheirCaps, pSBC, 4);
			m_bTheirsKnown = TRUE;
			Configure (FALSE);
		}
		else
		{
			m_nEndPoint++;
			NextEndPoint ();
		}
		break;
	}

	case SIG_SET_CONFIGURATION:
		if (bAccept)
		{
			m_Config = m_NewConfig;
			memcpy (m_ConfigBytes, m_NewBytes, 4);
			const u8 SEID = (u8) (m_nTheirSEID << 2);
			Enter (Opening);
			Command (SIG_OPEN, &SEID, 1);
		}
		else if (m_State == Configuring)	// (not this one: the next)
		{
			m_nEndPoint++;
			NextEndPoint ();
		}
		break;

	case SIG_OPEN:
		if (bAccept)
		{
			Enter (WaitMedia);
			m_pHost->OpenMedia ();
		}
		else
		{
			Fail ("the device doesn't open the stream");
		}
		break;

	case SIG_START:
		Enter (bAccept ? Streaming : Open);
		if (bAccept)
		{
			LOGNOTE ("The stream is started");
		}
		m_pHost->OnStream ();
		if (bAccept)
		{
			Go ();
		}
		break;

	case SIG_SUSPEND:
		Enter (bAccept ? Open : Streaming);
		m_pHost->OnStream ();
		if (bAccept)
		{
			LOGNOTE ("The stream is suspended");
			Go ();
		}
		break;

	case SIG_RECONFIGURE:
		if (bAccept)
		{
			m_Config = m_NewConfig;
			memcpy (m_ConfigBytes, m_NewBytes, 4);
			LOGNOTE ("The stream is at %u Hz now", m_Config.nRate);
		}
		else					// (it stays as it is: the sound is resampled)
		{
			m_nRate = m_Config.nRate;
		}
		Enter (Open);
		m_pHost->OnStream ();
		Go ();
		break;
	}
}

// the speaker's commands to our end point
void CBTAVDTP::OnCommand (u8 nLabel, u8 nSignal, const u8 *p, unsigned n)
{
	u8 Data[16];
	switch (nSignal)
	{
	case SIG_DISCOVER:				// our one end point: an audio source
		Data[0] = (u8) (OUR_SEID << 2 | (m_State >= Configuring && m_State != Failed ? 2 : 0));
		Data[1] = 0;
		Answer (nLabel, nSignal, TRUE, Data, 2);
		break;

	case SIG_GET_CAPABILITIES:
	case SIG_GET_ALL_CAPS:				// what it can send: SBC at 44.1 and 48 kHz, every kind of it
		Data[0] = CAT_MEDIA_TRANSPORT;
		Data[1] = 0;
		Data[2] = CAT_MEDIA_CODEC;
		Data[3] = 6;
		Data[4] = 0;
		Data[5] = 0;
		Data[6] = 0x3F;
		Data[7] = 0xFF;
		Data[8] = 2;
		Data[9] = MAX_BITPOOL;
		Answer (nLabel, nSignal, TRUE, Data, 10);
		break;

	case SIG_SET_CONFIGURATION:			// they configure the stream: our ID, theirs, the configuration
	case SIG_RECONFIGURE:
	{
		boolean bFirst = nSignal == SIG_SET_CONFIGURATION;
		const u8 *pSBC = n >= (bFirst ? 2U : 1U) ? FindSBC (p + (bFirst ? 2 : 1), n - (bFirst ? 2 : 1)) : nullptr;
		TConfig Config;
		if (bFirst && m_State >= Configuring && m_State != Failed)
		{
			Data[0] = CAT_MEDIA_CODEC;
			Data[1] = ERR_SEP_IN_USE;
			Answer (nLabel, nSignal, FALSE, Data, 2);
		}
		else if (!pSBC || !Parse (pSBC, &Config))
		{
			Data[0] = CAT_MEDIA_CODEC;
			Data[1] = ERR_UNSUPPORTED_CONFIG;
			Answer (nLabel, nSignal, FALSE, Data, 2);
		}
		else
		{
			m_Config = Config;
			memcpy (m_ConfigBytes, pSBC, 4);
			if (bFirst)
			{
				m_nTheirSEID = p[1] >> 2;
				m_nSignal = 0;		// (theirs it is: ours is off)
				Enter (Opening);
			}
			LOGNOTE ("The device configured the stream: %u Hz, bitpool %u", m_Config.nRate, m_Config.nBitpool);
			Answer (nLabel, nSignal, TRUE);
			m_pHost->OnStream ();
		}
		break;
	}

	case SIG_GET_CONFIGURATION:
		Data[0] = CAT_MEDIA_TRANSPORT;
		Data[1] = 0;
		Data[2] = CAT_MEDIA_CODEC;
		Data[3] = 6;
		Data[4] = 0;
		Data[5] = 0;
		memcpy (Data + 6, m_ConfigBytes, 4);
		Answer (nLabel, nSignal, TRUE, Data, 10);
		break;

	case SIG_OPEN:					// (its transport channel comes from them)
		Answer (nLabel, nSignal, TRUE);
		Enter (WaitMedia);
		break;

	case SIG_START:
		if (m_State == Open || m_State == Starting)
		{
			Answer (nLabel, nSignal, TRUE);
			m_nSignal = 0;
			Enter (Streaming);
			m_pHost->OnStream ();
		}
		else
		{
			Data[0] = n ? p[0] : 0;
			Data[1] = ERR_BAD_STATE;
			Answer (nLabel, nSignal, FALSE, Data, 2);
		}
		break;

	case SIG_SUSPEND:
		if (m_State == Streaming || m_State == Suspending)
		{
			Answer (nLabel, nSignal, TRUE);
			m_nSignal = 0;
			Enter (Open);
			m_pHost->OnStream ();
		}
		else
		{
			Data[0] = n ? p[0] : 0;
			Data[1] = ERR_BAD_STATE;
			Answer (nLabel, nSignal, FALSE, Data, 2);
		}
		break;

	case SIG_CLOSE:
	case SIG_ABORT:					// the stream is over (the channel for it too)
		Answer (nLabel, nSignal, TRUE);
		m_nSignal = 0;
		m_pHost->CloseMedia ();
		Enter (Failed);
		m_pHost->OnStream ();
		break;

	case SIG_DELAY_REPORT:				// (how late the speaker plays: taken, not used)
		Answer (nLabel, nSignal, TRUE);
		break;

	default:
		Data[0] = ERR_NOT_SUPPORTED;
		Answer (nLabel, nSignal, FALSE, Data, 1);
		break;
	}
}

void CBTAVDTP::Update (void)
{
	unsigned nNow = CTimer::GetClockTicks ();
	if (m_State == Waiting && nNow - m_nSince >= WAIT_US)
	{
		Enter (Discovering);
		Command (SIG_DISCOVER);
	}
	else if (m_State == Opening && !m_nSignal && nNow - m_nSince >= 3000000)
	{
		// (the device configured the stream and left it there: opened from here)
		const u8 SEID = (u8) (m_nTheirSEID << 2);
		Command (SIG_OPEN, &SEID, 1);
	}
	else if (m_nSignal && nNow - m_nSince >= ANSWER_TIMEOUT_US)
	{
		Fail ("the device doesn't answer");
	}
}

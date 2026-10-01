//
// bt_l2cap.cpp
//
#include "bt_l2cap.h"
#include <circle/logger.h>
#include <circle/timer.h>
#include <circle/util.h>

LOGMODULE ("btl2cap");

#define CID_SIGNALLING		0x0001
#define ANSWER_TIMEOUT_US	10000000

// the signalling's commands
#define SIG_REJECT		0x01
#define SIG_CONNECT_REQ		0x02
#define SIG_CONNECT_RSP		0x03
#define SIG_CONFIG_REQ		0x04
#define SIG_CONFIG_RSP		0x05
#define SIG_DISCONNECT_REQ	0x06
#define SIG_DISCONNECT_RSP	0x07
#define SIG_ECHO_REQ		0x08
#define SIG_ECHO_RSP		0x09
#define SIG_INFO_REQ		0x0A
#define SIG_INFO_RSP		0x0B

#define GET16(p)		((p)[0] | (p)[1] << 8)
#define PUT16(p, v)		((p)[0] = (u8) (v), (p)[1] = (u8) ((v) >> 8))

CBTL2CAP::CBTL2CAP (CBTHCI *pHCI, CBTL2CAPClient *pClient)
:	m_pHCI (pHCI),
	m_pClient (pClient),
	m_bLink (FALSE),
	m_nHandle (0),
	m_nId (0)
{
	memset (m_Channel, 0, sizeof m_Channel);
}

void CBTL2CAP::LinkUp (u16 nHandle)
{
	m_bLink = TRUE;
	m_nHandle = nHandle;
	memset (m_Channel, 0, sizeof m_Channel);
}

void CBTL2CAP::LinkDown (void)
{
	if (!m_bLink)
	{
		return;
	}
	m_bLink = FALSE;
	for (unsigned i = 0; i < Channels; i++)
	{
		if (m_Channel[i].State != Free)
		{
			Drop (i);
		}
	}
}

unsigned CBTL2CAP::ByOurCID (u16 nCID) const
{
	unsigned i = nCID - 0x40;
	return nCID >= 0x40 && i < Channels && m_Channel[i].State != Free ? i : None;
}

boolean CBTL2CAP::IsOpen (unsigned nChannel) const
{
	return nChannel < Channels && m_Channel[nChannel].State == Opened;
}

unsigned CBTL2CAP::GetMTU (unsigned nChannel) const
{
	return nChannel < Channels ? m_Channel[nChannel].nMTU : 0;
}

void CBTL2CAP::Drop (unsigned nChannel)
{
	u16 nPSM = m_Channel[nChannel].nPSM;
	m_Channel[nChannel].State = Free;
	m_pClient->OnChannelClosed (nChannel, nPSM);
}

void CBTL2CAP::Signal (u8 nCode, u8 nId, const u8 *pData, unsigned nBytes)
{
	u8 Frame[8 + 64];
	if (nBytes > 64 || !m_bLink)
	{
		return;
	}
	PUT16 (Frame, 4 + nBytes);
	PUT16 (Frame + 2, CID_SIGNALLING);
	Frame[4] = nCode;
	Frame[5] = nId;
	PUT16 (Frame + 6, nBytes);
	memcpy (Frame + 8, pData, nBytes);
	if (!m_pHCI->SendFrame (m_nHandle, Frame, 8 + nBytes))
	{
		LOGWARN ("Signal %02X dropped: no room", nCode);
	}
}

unsigned CBTL2CAP::Open (u16 nPSM)
{
	for (unsigned i = 0; i < Channels && m_bLink; i++)
	{
		TChannel &C = m_Channel[i];
		if (C.State == Free)
		{
			memset (&C, 0, sizeof C);
			C.State = Asking;
			C.nPSM = nPSM;
			C.nMTU = 672;			// (L2CAP's default, till they say)
			C.nSince = CTimer::GetClockTicks ();
			u8 Data[4];
			PUT16 (Data, nPSM);
			PUT16 (Data + 2, OurCID (i));
			m_nId = m_nId == 255 ? 1 : m_nId + 1;
			Signal (SIG_CONNECT_REQ, m_nId, Data, sizeof Data);
			return i;
		}
	}
	return None;
}

void CBTL2CAP::Close (unsigned nChannel)
{
	if (nChannel >= Channels || m_Channel[nChannel].State == Free || m_Channel[nChannel].State == Closing)
	{
		return;
	}
	TChannel &C = m_Channel[nChannel];
	if (C.State == Asking)				// (they don't know it yet)
	{
		Drop (nChannel);
		return;
	}
	u8 Data[4];
	PUT16 (Data, C.nTheirCID);
	PUT16 (Data + 2, OurCID (nChannel));
	m_nId = m_nId == 255 ? 1 : m_nId + 1;
	Signal (SIG_DISCONNECT_REQ, m_nId, Data, sizeof Data);
	C.State = Closing;
	C.nSince = CTimer::GetClockTicks ();
}

// our side of a channel's configuration: the frames we take
void CBTL2CAP::Configure (unsigned nChannel)
{
	TChannel &C = m_Channel[nChannel];
	C.State = Configuring;
	C.nSince = CTimer::GetClockTicks ();
	u8 Data[8];
	PUT16 (Data, C.nTheirCID);
	PUT16 (Data + 2, 0);
	Data[4] = 0x01;					// the option: our MTU
	Data[5] = 2;
	PUT16 (Data + 6, OurMTU);
	m_nId = m_nId == 255 ? 1 : m_nId + 1;
	Signal (SIG_CONFIG_REQ, m_nId, Data, sizeof Data);
}

void CBTL2CAP::Check (unsigned nChannel)
{
	TChannel &C = m_Channel[nChannel];
	if (C.State == Configuring && C.bOursDone && C.bTheirsDone)
	{
		C.State = Opened;
		m_pClient->OnChannelOpen (nChannel, C.nPSM, C.bTheirs);
	}
}

boolean CBTL2CAP::Send (unsigned nChannel, const u8 *pData, unsigned nBytes)
{
	if (!IsOpen (nChannel) || nBytes > m_Channel[nChannel].nMTU || nBytes + 4 > CBTHCI::MaxFrame)
	{
		return FALSE;
	}
	u8 Frame[CBTHCI::MaxFrame];
	PUT16 (Frame, nBytes);
	PUT16 (Frame + 2, m_Channel[nChannel].nTheirCID);
	memcpy (Frame + 4, pData, nBytes);
	return m_pHCI->SendFrame (m_nHandle, Frame, 4 + nBytes);
}

void CBTL2CAP::OnFrame (const u8 *pFrame, unsigned nBytes)
{
	if (nBytes < 4 || !m_bLink)
	{
		return;
	}
	u16 nCID = GET16 (pFrame + 2);
	const u8 *p = pFrame + 4;
	unsigned n = nBytes - 4;
	if (nCID == CID_SIGNALLING)			// commands, one after another
	{
		while (n >= 4)
		{
			unsigned nLength = GET16 (p + 2);
			if (4 + nLength > n)
			{
				break;
			}
			OnSignal (p[0], p[1], p + 4, nLength);
			p += 4 + nLength;
			n -= 4 + nLength;
		}
		return;
	}
	unsigned i = ByOurCID (nCID);
	if (i != None && (m_Channel[i].State == Opened || m_Channel[i].State == Configuring))
	{
		m_pClient->OnChannelData (i, m_Channel[i].nPSM, p, n);
	}
}

void CBTL2CAP::OnSignal (u8 nCode, u8 nId, const u8 *p, unsigned n)
{
	u8 Data[32];
	switch (nCode)
	{
	case SIG_CONNECT_REQ:				// the PSM, their channel
		if (n >= 4)
		{
			u16 nPSM = GET16 (p), nResult = 0x0002;		// (no such service here)
			unsigned i = None;
			if (m_pClient->OnChannelAsked (nPSM))
			{
				nResult = 0x0004;			// (no channel left)
				for (unsigned k = 0; k < Channels && i == None; k++)
				{
					if (m_Channel[k].State == Free)
					{
						i = k;
						nResult = 0;
					}
				}
			}
			PUT16 (Data, i != None ? OurCID (i) : 0);
			PUT16 (Data + 2, GET16 (p + 2));
			PUT16 (Data + 4, nResult);
			PUT16 (Data + 6, 0);
			Signal (SIG_CONNECT_RSP, nId, Data, 8);
			if (i != None)
			{
				TChannel &C = m_Channel[i];
				memset (&C, 0, sizeof C);
				C.nPSM = nPSM;
				C.nTheirCID = GET16 (p + 2);
				C.bTheirs = TRUE;
				C.nMTU = 672;
				Configure (i);
			}
		}
		break;

	case SIG_CONNECT_RSP:				// their channel, ours, the result
		if (n >= 8)
		{
			unsigned i = ByOurCID (GET16 (p + 2));
			u16 nResult = GET16 (p + 4);
			if (i == None || m_Channel[i].State != Asking)
			{
				break;
			}
			if (nResult == 0)
			{
				m_Channel[i].nTheirCID = GET16 (p);
				Configure (i);
			}
			else if (nResult == 1)		// (pending: more will come)
			{
				m_Channel[i].nSince = CTimer::GetClockTicks ();
			}
			else
			{
				LOGNOTE ("PSM %04X refused: result %u", m_Channel[i].nPSM, nResult);
				Drop (i);
			}
		}
		break;

	case SIG_CONFIG_REQ:				// our channel, flags, their options
		if (n >= 4)
		{
			unsigned i = ByOurCID (GET16 (p));
			if (i == None)
			{
				PUT16 (Data, 0x0002);		// (no such channel)
				PUT16 (Data + 2, GET16 (p));
				PUT16 (Data + 4, 0);
				Signal (SIG_REJECT, nId, Data, 6);
				break;
			}
			TChannel &C = m_Channel[i];
			u16 nResult = 0;
			unsigned nReply = 6;
			for (unsigned k = 4; k + 2 <= n && nResult == 0; k += 2 + p[k + 1])
			{
				unsigned nType = p[k] & 0x7F, nLength = p[k + 1];
				if (k + 2 + nLength > n)
				{
					break;
				}
				if (nType == 0x01 && nLength == 2)		// the frames they take
				{
					C.nMTU = GET16 (p + k + 2);
				}
				else if (nType == 0x04 && nLength == 9 && p[k + 2] != 0)
				{
					// retransmission or streaming mode: basic mode it is
					nResult = 0x0001;
					Data[6] = 0x04;
					Data[7] = 9;
					memset (Data + 8, 0, 9);
					nReply = 6 + 11;
				}
				else if (nType > 0x05 && !(p[k] & 0x80))	// (not one to pass over)
				{
					nResult = 0x0003;
				}
			}
			PUT16 (Data, C.nTheirCID);
			PUT16 (Data + 2, 0);
			PUT16 (Data + 4, nResult);
			Signal (SIG_CONFIG_RSP, nId, Data, nReply);
			if (nResult == 0 && !(GET16 (p + 2) & 1))		// (all of it said)
			{
				C.bTheirsDone = TRUE;
				Check (i);
			}
		}
		break;

	case SIG_CONFIG_RSP:				// our channel, flags, the result
		if (n >= 6)
		{
			unsigned i = ByOurCID (GET16 (p));
			if (i == None || m_Channel[i].State != Configuring)
			{
				break;
			}
			if (GET16 (p + 4) == 0)
			{
				m_Channel[i].bOursDone = TRUE;
				Check (i);
			}
			else
			{
				LOGNOTE ("PSM %04X: our configuration refused (result %u)", m_Channel[i].nPSM, GET16 (p + 4));
				Close (i);
			}
		}
		break;

	case SIG_DISCONNECT_REQ:			// our channel, theirs
		if (n >= 4)
		{
			unsigned i = ByOurCID (GET16 (p));
			memcpy (Data, p, 4);
			Signal (SIG_DISCONNECT_RSP, nId, Data, 4);
			if (i != None)
			{
				Drop (i);
			}
		}
		break;

	case SIG_DISCONNECT_RSP:			// their channel, ours
		if (n >= 4)
		{
			unsigned i = ByOurCID (GET16 (p + 2));
			if (i != None && m_Channel[i].State == Closing)
			{
				Drop (i);
			}
		}
		break;

	case SIG_ECHO_REQ:
		Signal (SIG_ECHO_RSP, nId, p, n < 32 ? n : 32);
		break;

	case SIG_INFO_REQ:				// what we can do
		if (n >= 2)
		{
			u16 nType = GET16 (p);
			PUT16 (Data, nType);
			PUT16 (Data + 2, 0);
			if (nType == 0x0002)		// features: fixed channels, no more
			{
				Data[4] = 0x80;
				Data[5] = Data[6] = Data[7] = 0;
				Signal (SIG_INFO_RSP, nId, Data, 8);
			}
			else if (nType == 0x0003)	// the fixed channels: the signalling
			{
				memset (Data + 4, 0, 8);
				Data[4] = 0x02;
				Signal (SIG_INFO_RSP, nId, Data, 12);
			}
			else
			{
				PUT16 (Data + 2, 1);	// (not known here)
				Signal (SIG_INFO_RSP, nId, Data, 4);
			}
		}
		break;

	case SIG_REJECT:
		LOGNOTE ("A command of ours rejected (reason %u)", n >= 2 ? GET16 (p) : 0);
		break;

	case SIG_ECHO_RSP:
	case SIG_INFO_RSP:
		break;

	default:
		PUT16 (Data, 0);			// (not understood)
		Signal (SIG_REJECT, nId, Data, 2);
		break;
	}
}

void CBTL2CAP::Update (void)
{
	unsigned nNow = CTimer::GetClockTicks ();
	for (unsigned i = 0; i < Channels && m_bLink; i++)
	{
		TChannel &C = m_Channel[i];
		if (C.State != Free && C.State != Opened && nNow - C.nSince > ANSWER_TIMEOUT_US)
		{
			LOGNOTE ("PSM %04X: no answer", C.nPSM);
			Drop (i);
		}
	}
}

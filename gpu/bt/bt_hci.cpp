//
// bt_hci.cpp
//
#include "bt_hci.h"
#include <circle/logger.h>
#include <circle/string.h>
#include <circle/util.h>

LOGMODULE ("bthci");

#define H4_COMMAND	0x01
#define H4_ACL		0x02
#define H4_EVENT	0x04

CBTHCI::CBTHCI (CBTUart *pUart, CBTHCIClient *pClient)
:	m_pUart (pUart),
	m_pClient (pClient),
	m_bTrace (FALSE),
	m_pCommand (nullptr),
	m_pPacket (nullptr)
{
	Restart ();
}

void CBTHCI::Restart (void)
{
	if (!m_pCommand)
	{
		m_pCommand = new u8[Commands][CommandBytes];
		m_pPacket = new TPacket[Packets];
	}
	m_nRx = 0;
	m_nRxNeed = 1;
	m_nSkipped = 0;
	m_nFrame = m_nFrameNeed = 0;
	m_nFrameHandle = 0;
	m_nCommandIn = m_nCommandOut = 0;
	m_nCommandCredits = 1;
	m_nPacketIn = m_nPacketOut = 0;
	m_nPacketBytes = 27;
	m_nBuffers = 1;
	m_nPending = 0;
}

void CBTHCI::SetBuffers (unsigned nPacketBytes, unsigned nPackets)
{
	m_nPacketBytes = nPacketBytes > PacketBytes - 5 ? PacketBytes - 5 : nPacketBytes;
	m_nBuffers = nPackets;
}

void CBTHCI::Trace (const char *pWhat, const u8 *p, unsigned nBytes)
{
	if (!m_bTrace)
	{
		return;
	}
	CString Text, Byte;
	for (unsigned i = 0; i < nBytes && i < 40; i++)
	{
		Byte.Format ("%02X ", p[i]);
		Text.Append (Byte);
	}
	LOGNOTE ("%s %u: %s%s", pWhat, nBytes, (const char *) Text, nBytes > 40 ? "..." : "");
}

void CBTHCI::Update (void)
{
	Receive ();
	Send ();
}

// the bytes received: a packet's type, its header (which says how long it is), the rest
void CBTHCI::Receive (void)
{
	for (;;)
	{
		unsigned nGot = m_pUart->Read (m_Rx + m_nRx, m_nRxNeed - m_nRx);
		m_nRx += nGot;
		if (m_nRx < m_nRxNeed)
		{
			return;
		}
		if (m_nRx == 1)
		{
			if (m_Rx[0] == H4_EVENT)
			{
				m_nRxNeed = 3;
			}
			else if (m_Rx[0] == H4_ACL)
			{
				m_nRxNeed = 5;
			}
			else				// not a packet's start: on to the next byte
			{
				m_nSkipped++;
				m_nRx = 0;
			}
			continue;
		}
		unsigned nHeader = m_Rx[0] == H4_EVENT ? 3 : 5;
		if (m_nRx == nHeader)
		{
			unsigned nLength = m_Rx[0] == H4_EVENT ? m_Rx[2] : m_Rx[3] | m_Rx[4] << 8;
			if (nLength > 1024)		// (not a packet: lost in the stream)
			{
				m_nSkipped += m_nRx;
				m_nRx = 0;
				m_nRxNeed = 1;
				continue;
			}
			m_nRxNeed = nHeader + nLength;
			if (nLength)
			{
				continue;
			}
		}
		if (m_nSkipped)
		{
			LOGWARN ("%u bytes that were no packet", m_nSkipped);
			m_nSkipped = 0;
		}
		Packet ();
		m_nRx = 0;
		m_nRxNeed = 1;
	}
}

void CBTHCI::Packet (void)
{
	if (m_Rx[0] == H4_EVENT)
	{
		u8 nCode = m_Rx[1];
		const u8 *p = m_Rx + 3;
		unsigned nBytes = m_Rx[2];
		if (nCode != HCI_EV_NUMBER_OF_COMPLETED_PACKETS)
		{
			Trace ("event", m_Rx + 1, m_nRx - 1);
		}
		if (nCode == HCI_EV_COMMAND_COMPLETE && nBytes >= 3)
		{
			m_nCommandCredits = p[0];
		}
		else if (nCode == HCI_EV_COMMAND_STATUS && nBytes >= 4)
		{
			m_nCommandCredits = p[1];
		}
		else if (nCode == HCI_EV_NUMBER_OF_COMPLETED_PACKETS && nBytes >= 1)
		{
			for (unsigned i = 0; i < p[0] && 1 + 4 * i + 3 < nBytes; i++)
			{
				unsigned nDone = p[1 + 4 * i + 2] | p[1 + 4 * i + 3] << 8;
				m_nPending = nDone < m_nPending ? m_nPending - nDone : 0;
			}
		}
		m_pClient->OnEvent (nCode, p, nBytes);
		return;
	}

	// data: a frame's first packet (its header says how long the frame is), or more of it
	u16 nHandle = (m_Rx[1] | m_Rx[2] << 8) & 0x0FFF;
	unsigned nBoundary = m_Rx[2] >> 4 & 3;
	const u8 *p = m_Rx + 5;
	unsigned nBytes = m_nRx - 5;
	if (nBoundary != 1)
	{
		if (m_nFrame)
		{
			LOGWARN ("A frame cut short (%u of %u bytes)", m_nFrame, m_nFrameNeed);
		}
		m_nFrame = 0;
		if (nBytes < 4)
		{
			return;
		}
		m_nFrameNeed = 4 + (p[0] | p[1] << 8);
		m_nFrameHandle = nHandle;
	}
	else if (!m_nFrame || nHandle != m_nFrameHandle)
	{
		return;				// (more of a frame whose start wasn't seen)
	}
	if (m_nFrameNeed > MaxFrame || m_nFrame + nBytes > m_nFrameNeed)
	{
		LOGWARN ("A frame of %u bytes dropped", m_nFrameNeed);
		m_nFrame = 0;
		return;
	}
	memcpy (m_Frame + m_nFrame, p, nBytes);
	m_nFrame += nBytes;
	if (m_nFrame == m_nFrameNeed)
	{
		Trace ("frame in", m_Frame, m_nFrame);
		m_nFrame = 0;
		m_pClient->OnFrame (nHandle, m_Frame, m_nFrameNeed);
	}
}

boolean CBTHCI::Command (u16 nOpcode, const void *pParams, unsigned nBytes)
{
	if (m_nCommandIn - m_nCommandOut == Commands || nBytes > 255)
	{
		LOGWARN ("Command %04X dropped: %u are waiting", nOpcode, Commands);
		return FALSE;
	}
	u8 *p = m_pCommand[m_nCommandIn % Commands];
	p[0] = H4_COMMAND;
	p[1] = (u8) nOpcode;
	p[2] = (u8) (nOpcode >> 8);
	p[3] = (u8) nBytes;
	if (nBytes)
	{
		memcpy (p + 4, pParams, nBytes);
	}
	m_nCommandIn++;
	Send ();
	return TRUE;
}

unsigned CBTHCI::GetRoom (unsigned nBytes) const
{
	unsigned nPackets = (nBytes + m_nPacketBytes - 1) / m_nPacketBytes;
	return m_nPacketIn - m_nPacketOut + nPackets <= Packets ? nPackets : 0;
}

boolean CBTHCI::SendFrame (u16 nHandle, const u8 *pFrame, unsigned nBytes)
{
	if (!GetRoom (nBytes))
	{
		return FALSE;
	}
	Trace ("frame out", pFrame, nBytes);
	for (unsigned nOffset = 0; nOffset < nBytes; nOffset += m_nPacketBytes)
	{
		unsigned n = nBytes - nOffset < m_nPacketBytes ? nBytes - nOffset : m_nPacketBytes;
		TPacket *p = &m_pPacket[m_nPacketIn % Packets];
		u16 nFlags = nHandle | (nOffset ? 1 : 2) << 12;		// (more of it; its start, which may be flushed)
		p->Data[0] = H4_ACL;
		p->Data[1] = (u8) nFlags;
		p->Data[2] = (u8) (nFlags >> 8);
		p->Data[3] = (u8) n;
		p->Data[4] = (u8) (n >> 8);
		memcpy (p->Data + 5, pFrame + nOffset, n);
		p->nBytes = 5 + n;
		m_nPacketIn++;
	}
	Send ();
	return TRUE;
}

void CBTHCI::Disconnected (u16 nHandle)
{
	m_nPending = 0;
	m_nPacketOut = m_nPacketIn;
	m_nFrame = 0;
}

void CBTHCI::Send (void)
{
	while (m_nCommandIn != m_nCommandOut && m_nCommandCredits)
	{
		const u8 *p = m_pCommand[m_nCommandOut % Commands];
		if (!m_pUart->Write (p, 4 + p[3]))
		{
			return;
		}
		Trace ("command", p + 1, 3 + p[3]);
		m_nCommandOut++;
		m_nCommandCredits--;
	}
	while (m_nPacketIn != m_nPacketOut && m_nPending < m_nBuffers)
	{
		const TPacket *p = &m_pPacket[m_nPacketOut % Packets];
		if (!m_pUart->Write (p->Data, p->nBytes))
		{
			return;
		}
		m_nPacketOut++;
		m_nPending++;
	}
}

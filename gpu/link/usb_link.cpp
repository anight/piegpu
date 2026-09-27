//
// usb_link.cpp
//
#include "usb_link.h"
#include <pgpu_protocol.h>
#include <circle/util.h>

CUSBLink::CUSBLink (CDevLink *pDevLink)
:	m_pDevLink (pDevLink),
	m_pBuffer (nullptr),
	m_nBytes (0),
	m_nStart (0),
	m_nConsumed (0),
	m_nCRCErrors (0),
	m_nCredited (0)
{
	for (u32 i = 0; i < 256; i++)
	{
		u32 c = i;
		for (unsigned k = 0; k < 8; k++)
		{
			c = c & 1 ? PGPU_CRC_POLY_REFLECTED ^ (c >> 1) : c >> 1;
		}
		m_CRCTable[i] = c;
	}
}

CUSBLink::~CUSBLink (void)
{
	delete [] m_pBuffer;
}

boolean CUSBLink::Initialize (void)
{
	m_pBuffer = new u8[BufferBytes];

	return m_pBuffer != nullptr;
}

boolean CUSBLink::IsActive (void)
{
	return m_pDevLink->IsStreaming ();
}

// flow control: the host may send what the gadget's queue holds beyond what
// we have taken
void CUSBLink::Update (void)
{
	if (!IsActive ())
	{
		return;
	}

	u32 nReceived = m_pDevLink->GetStreamReceived ();
	if (nReceived != m_nCredited)
	{
		SendReply (PGPU_REPLY_CREDIT, &nReceived, 1);
		m_nCredited = nReceived;
	}
}

u32 CUSBLink::CRC (u32 nCRC, u32 nWord) const
{
	for (unsigned b = 0; b < 4; b++, nWord >>= 8)		// little endian bytes
	{
		nCRC = m_CRCTable[(nCRC ^ nWord) & 0xFF] ^ (nCRC >> 8);
	}

	return nCRC;
}

// The stream is byte aligned but may start with text (the log before the
// switch): a packet is a header with the command SYNC byte and a valid length,
// and the CRC over it. Anything else is skipped a byte at a time.
const u32 *CUSBLink::GetPacket (u32 *pHeader)
{
	if (m_nConsumed)
	{
		m_nStart += m_nConsumed;
		m_nConsumed = 0;
	}

	while (TRUE)
	{
		if (m_nStart > BufferBytes / 2)
		{
			memmove (m_pBuffer, m_pBuffer + m_nStart, m_nBytes - m_nStart);	// compact
			m_nBytes -= m_nStart;
			m_nStart = 0;
		}
		m_nBytes += m_pDevLink->StreamRead (m_pBuffer + m_nBytes, BufferBytes - m_nBytes);
		if (m_nBytes - m_nStart < 8)
		{
			return nullptr;
		}

		const u8 *p = m_pBuffer + m_nStart;
		u32 nHeader;
		memcpy (&nHeader, p, 4);
		unsigned nLength = PGPU_HEADER_LEN (nHeader);
		if (   PGPU_HEADER_SYNC (nHeader) != PGPU_SYNC_COMMAND
		    || nLength > PGPU_MAX_PAYLOAD)
		{
			m_nStart++;					// not a header
			continue;
		}

		unsigned nPacket = (nLength + 2) * 4;
		if (m_nBytes - m_nStart < nPacket)
		{
			if (m_nStart + nPacket > BufferBytes)
			{
				memmove (m_pBuffer, p, m_nBytes - m_nStart);	// room for the rest
				m_nBytes -= m_nStart;
				m_nStart = 0;
			}
			return nullptr;					// wait for the rest
		}

		u32 nCRC = PGPU_CRC_INIT;
		for (unsigned i = 0; i < nLength + 1; i++)
		{
			u32 nWord;
			memcpy (&nWord, p + i * 4, 4);
			nCRC = CRC (nCRC, nWord);
		}
		u32 nExpected;
		memcpy (&nExpected, p + (nLength + 1) * 4, 4);
		if ((nCRC ^ 0xFFFFFFFF) != nExpected)
		{
			m_nCRCErrors++;
			m_nStart++;
			continue;
		}

		// the payload must be word aligned for the command parser
		if ((m_nStart & 3) != 0)
		{
			memmove (m_pBuffer, p, m_nBytes - m_nStart);
			m_nBytes -= m_nStart;
			m_nStart = 0;
			p = m_pBuffer;
		}

		*pHeader = nHeader;
		m_nConsumed = nPacket;

		return (const u32 *) (p + 4);
	}
}

boolean CUSBLink::SendReply (u8 uchOpcode, const u32 *pPayload, unsigned nLength)
{
	u32 Packet[2 + 64];
	if (nLength > 64)
	{
		return FALSE;
	}

	Packet[0] = PGPU_HEADER (PGPU_SYNC_REPLY, uchOpcode, nLength);
	u32 nCRC = CRC (PGPU_CRC_INIT, Packet[0]);
	for (unsigned i = 0; i < nLength; i++)
	{
		Packet[1 + i] = pPayload[i];
		nCRC = CRC (nCRC, pPayload[i]);
	}
	Packet[1 + nLength] = nCRC ^ 0xFFFFFFFF;

	return m_pDevLink->Write (Packet, (nLength + 2) * 4);
}

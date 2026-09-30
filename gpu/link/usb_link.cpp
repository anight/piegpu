//
// usb_link.cpp
//
#include "usb_link.h"
#include <pgpu_protocol.h>

CUSBLink::CUSBLink (CDevLink *pDevLink)
:	m_pDevLink (pDevLink),
	m_nCredited (0)
{
}

CUSBLink::~CUSBLink (void)
{
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

// STREAM_END: the serial port carries text again (the log, the installer)
void CUSBLink::EndSession (void)
{
	DiscardBuffered ();
	m_pDevLink->EndStream ();
	m_nCredited = 0;
}

unsigned CUSBLink::ReadStream (void *pBuffer, unsigned nMax)
{
	return m_pDevLink->StreamRead (pBuffer, nMax);
}

boolean CUSBLink::WriteStream (const void *pData, unsigned nLength)
{
	return m_pDevLink->Write (pData, nLength);
}

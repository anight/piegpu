//
// devlink.cpp
//
#include "devlink.h"
#include <circle/devicenameservice.h>
#include <circle/logger.h>
#include <circle/startup.h>
#include <assert.h>

LOGMODULE ("devlink");

CDevLink::CDevLink (CInterruptSystem *pInterrupt)
:	m_Gadget (pInterrupt, DEVLINK_USB_VENDOR_ID, DEVLINK_USB_PRODUCT_ID),
	m_pSerial (nullptr),
	m_pPrevLogTarget (nullptr),
	m_bHostActive (FALSE),
	m_bReplayDone (FALSE),
	m_pMagicPtr (DEVLINK_REBOOT_MAGIC),
	m_nRxIn (0),
	m_nRxOut (0)
{
}

CDevLink::~CDevLink (void)
{
}

boolean CDevLink::Initialize (void)
{
	m_Watchdog.Start (DEVLINK_WATCHDOG_SECONDS);

	return m_Gadget.Initialize ();
}

void CDevLink::Update (void)
{
	m_Watchdog.Start (DEVLINK_WATCHDOG_SECONDS);	// re-trigger

	if (   m_Gadget.UpdatePlugAndPlay ()
	    && !m_pSerial)
	{
		m_pSerial = static_cast<CUSBSerialDevice *> (
			CDeviceNameService::Get ()->GetDevice ("utty1", FALSE));
		if (m_pSerial)
		{
			m_pSerial->RegisterRemovedHandler (DeviceRemovedHandler, this);
			m_pSerial->SetOptions (SERIAL_OPTION_ONLCR);
		}
	}

	if (!m_pSerial)
	{
		return;
	}

	char Buffer[256];
	int nResult = m_pSerial->Read (Buffer, sizeof Buffer);
	if (nResult > 0)
	{
		// The host has opened the port and is listening now
		m_bHostActive = TRUE;

		CheckMagic (Buffer, nResult);

		for (int i = 0; i < nResult; i++)
		{
			unsigned nNext = (m_nRxIn + 1) % RxBufferSize;
			if (nNext != m_nRxOut)		// drop when full
			{
				m_RxBuffer[m_nRxIn] = Buffer[i];
				m_nRxIn = nNext;
			}
		}
	}

	if (   m_bHostActive
	    && !m_bReplayDone)
	{
		// Replay the log ring buffer in small chunks, so that the gadget's
		// (non-blocking) send queue does not overflow. Keep the log target
		// unchanged until done, so that new messages go into the ring buffer.
		CLogger *pLogger = CLogger::Get ();
		nResult = pLogger->Read (Buffer, sizeof Buffer);
		if (nResult > 0)
		{
			m_pSerial->Write (Buffer, nResult);
		}
		else
		{
			m_pPrevLogTarget = pLogger->GetTarget ();
			pLogger->SetNewTarget (m_pSerial);

			m_bReplayDone = TRUE;

			LOGNOTE ("Host connected");
		}
	}
}

int CDevLink::GetChar (void)
{
	if (m_nRxOut == m_nRxIn)
	{
		return -1;
	}

	char c = m_RxBuffer[m_nRxOut];
	m_nRxOut = (m_nRxOut + 1) % RxBufferSize;

	return (unsigned char) c;
}

void CDevLink::CheckMagic (const char *pData, unsigned nLength)
{
	while (nLength--)
	{
		char c = *pData++;

		if (c != *m_pMagicPtr)
		{
			m_pMagicPtr = DEVLINK_REBOOT_MAGIC;
		}

		if (c == *m_pMagicPtr)
		{
			if (*++m_pMagicPtr == '\0')
			{
				LOGNOTE ("Reboot requested by host");

				reboot ();
			}
		}
	}
}

void CDevLink::DeviceRemovedHandler (CDevice *pDevice, void *pContext)
{
	CDevLink *pThis = static_cast<CDevLink *> (pContext);
	assert (pThis);

	if (pThis->m_bReplayDone)
	{
		CLogger::Get ()->SetNewTarget (pThis->m_pPrevLogTarget);
	}

	pThis->m_pSerial = nullptr;
	pThis->m_bHostActive = FALSE;
	pThis->m_bReplayDone = FALSE;
}

boolean CDevLink::Write (const void *pData, unsigned nLength)
{
	const u8 *p = (const u8 *) pData;
	unsigned nStart = CTimer::GetClockTicks ();
	boolean bOK = TRUE;

	if (!m_pSerial)
	{
		return FALSE;
	}

	// raw: with ONLCR, Write() returns the length after the \n -> \r\n expansion
	m_pSerial->SetOptions (0);

	while (nLength)
	{
		int nResult = m_pSerial->Write (p, nLength);
		if (   nResult < 0
		    || CTimer::GetClockTicks () - nStart > 1000000)
		{
			bOK = FALSE;
			break;
		}
		assert ((unsigned) nResult <= nLength);
		p += nResult;
		nLength -= nResult;
	}

	m_pSerial->SetOptions (SERIAL_OPTION_ONLCR);

	return bOK;
}

//
// pgpugadget.cpp
//
// The serial port's part follows Circle's CUSBCDCGadget (lib/usb/gadget/
// usbcdcgadget.cpp: its descriptors and endpoints), with an interface
// association in front, as composite devices have it.
//
#include "pgpugadget.h"
#include <circle/bcmpropertytags.h>
#include <circle/string.h>
#include <circle/util.h>
#include <assert.h>

#define DESCRIPTOR_INTERFACE_ASSOCIATION	0x0B

// the bulk OUT endpoint receives at most this much a transfer (the DWC2's
// transfer size and packet count fields)
#define MAX_CHUNK				(64 * 1024)

const TUSBDeviceDescriptor CPicoGPUGadget::s_DeviceDescriptor =
{
	sizeof (TUSBDeviceDescriptor),
	DESCRIPTOR_DEVICE,
	0x200,				// bcdUSB
	0xEF, 2, 1,			// composite, with interface associations
	64,				// bMaxPacketSize0
	PGPU_GADGET_VENDOR_ID,
	PGPU_GADGET_PRODUCT_ID,
	0x100,				// bcdDevice
	1, 2, 3,			// strings: manufacturer, product, serial number
	1				// bNumConfigurations
};

const CPicoGPUGadget::TConfigurationDescriptor CPicoGPUGadget::s_ConfigurationDescriptor =
{
	{
		sizeof (TUSBConfigurationDescriptor),
		DESCRIPTOR_CONFIGURATION,
		sizeof s_ConfigurationDescriptor,
		3,			// bNumInterfaces
		1,			// bConfigurationValue
		0,
		0x80,			// bmAttributes (bus-powered)
		250			// bMaxPower (500 mA)
	},
	{
		sizeof (TInterfaceAssociationDescriptor),
		DESCRIPTOR_INTERFACE_ASSOCIATION,
		0, 2,			// interfaces 0 and 1
		2, 2, 1,		// CDC ACM
		0
	},
	{
		sizeof (TUSBInterfaceDescriptor),
		DESCRIPTOR_INTERFACE,
		0, 0,			// bInterfaceNumber, bAlternateSetting
		1,			// bNumEndpoints
		2, 2, 1,		// CDC ACM
		0
	},
	{
		5, DESCRIPTOR_CS_INTERFACE, 0, 0x110,	// header
		4, DESCRIPTOR_CS_INTERFACE, 2, 0,	// abstract control management
		5, DESCRIPTOR_CS_INTERFACE, 6, 0, 1	// union: 0 controls 1
	},
	{
		// never used, but Linux' cdc_acm wants it (Circle's CDC gadget
		// doesn't create it either)
		sizeof (TUSBEndpointDescriptor),
		DESCRIPTOR_ENDPOINT,
		EPNotif | 0x80,
		3,			// interrupt
		10, 11
	},
	{
		sizeof (TUSBInterfaceDescriptor),
		DESCRIPTOR_INTERFACE,
		1, 0,
		2,
		0x0A, 0, 0,		// CDC data
		0
	},
	{
		sizeof (TUSBEndpointDescriptor), DESCRIPTOR_ENDPOINT, EPSerialOut, 2, 512, 0
	},
	{
		sizeof (TUSBEndpointDescriptor), DESCRIPTOR_ENDPOINT, EPSerialIn | 0x80, 2, 512, 0
	},
	{
		sizeof (TUSBInterfaceDescriptor),
		DESCRIPTOR_INTERFACE,
		PGPU_GADGET_FUNCTION_INTERFACE, 0,
		1,
		0xFF, 0, 0,		// vendor specific (GUD)
		4			// iInterface
	},
	{
		sizeof (TUSBEndpointDescriptor), DESCRIPTOR_ENDPOINT, EPFunctionOut, 2, 512, 0
	}
};

const char *const CPicoGPUGadget::s_StringDescriptor[] =
{
	"\x04\x03\x09\x04",		// language ID: English (US)
	"pico-gpu",
	"pico-gpu",
	nullptr,			// the serial number: the board's
	"pico-gpu display"
};

CPicoGPUGadget::CPicoGPUGadget (CInterruptSystem *pInterrupt)
:	CDWUSBGadget (pInterrupt, HighSpeed),
	m_pFunction (nullptr),
	m_pSerial (nullptr),
	m_pSerialEP {nullptr, nullptr},
	m_pFunctionEP (nullptr)
{
	strcpy (m_SerialNumber, "0");
	CBcmPropertyTags Tags;
	TPropertyTagSerial Serial;
	if (Tags.GetTag (PROPTAG_GET_BOARD_SERIAL, &Serial, sizeof Serial))
	{
		CString Number;
		Number.Format ("%08X%08X", Serial.Serial[1], Serial.Serial[0]);
		strncpy (m_SerialNumber, Number, sizeof m_SerialNumber - 1);
		m_SerialNumber[sizeof m_SerialNumber - 1] = '\0';
	}
}

CPicoGPUGadget::~CPicoGPUGadget (void)
{
	assert (0);
}

const void *CPicoGPUGadget::GetDescriptor (u16 wValue, u16 wIndex, size_t *pLength)
{
	assert (pLength);
	u8 uchIndex = wValue & 0xFF;

	switch (wValue >> 8)
	{
	case DESCRIPTOR_DEVICE:
		if (!uchIndex)
		{
			*pLength = sizeof s_DeviceDescriptor;
			return &s_DeviceDescriptor;
		}
		break;

	case DESCRIPTOR_CONFIGURATION:
		if (!uchIndex)
		{
			*pLength = sizeof s_ConfigurationDescriptor;
			return &s_ConfigurationDescriptor;
		}
		break;

	case DESCRIPTOR_STRING:
		if (!uchIndex)
		{
			*pLength = (u8) s_StringDescriptor[0][0];
			return s_StringDescriptor[0];
		}
		if (uchIndex == 3)
		{
			return ToStringDescriptor (m_SerialNumber, pLength);
		}
		if (uchIndex < sizeof s_StringDescriptor / sizeof s_StringDescriptor[0])
		{
			return ToStringDescriptor (s_StringDescriptor[uchIndex], pLength);
		}
		break;

	default:
		break;
	}

	return nullptr;
}

const void *CPicoGPUGadget::ToStringDescriptor (const char *pString, size_t *pLength)
{
	size_t nLength = 2;
	for (u8 *p = m_StringDescriptorBuffer + 2; *pString; pString++)
	{
		assert (nLength < sizeof m_StringDescriptorBuffer - 1);
		*p++ = (u8) *pString;			// UTF-16
		*p++ = '\0';
		nLength += 2;
	}
	m_StringDescriptorBuffer[0] = (u8) nLength;
	m_StringDescriptorBuffer[1] = DESCRIPTOR_STRING;
	*pLength = nLength;

	return m_StringDescriptorBuffer;
}

int CPicoGPUGadget::OnClassOrVendorRequest (const TSetupData *pSetupData, u8 *pData)
{
	if (   (pSetupData->bmRequestType & 0x60) == 0x40			// vendor
	    && (pSetupData->bmRequestType & 0x1F) == 0x01			// interface
	    && (pSetupData->wIndex & 0xFF) == PGPU_GADGET_FUNCTION_INTERFACE
	    && m_pFunction)
	{
		return m_pFunction->OnVendorRequest (pSetupData, pData, 512);	// (EP0's buffer)
	}

	return CDWUSBGadget::OnClassOrVendorRequest (pSetupData, pData);	// (the serial port's)
}

void CPicoGPUGadget::AddEndpoints (void)
{
	assert (!m_pSerialEP[0] && !m_pSerialEP[1] && !m_pFunctionEP);
	m_pSerialEP[0] = new CUSBCDCGadgetEndpoint (&s_ConfigurationDescriptor.SerialOut, this);
	m_pSerialEP[1] = new CUSBCDCGadgetEndpoint (&s_ConfigurationDescriptor.SerialIn, this);
	m_pFunctionEP = new CBulkOutEndpoint (&s_ConfigurationDescriptor.FunctionOut, this);
}

void CPicoGPUGadget::CreateDevice (void)
{
	assert (!m_pSerial);
	m_pSerial = new CUSBSerialDevice;
	m_pSerialEP[0]->AttachInterface (m_pSerial);
	m_pSerialEP[1]->AttachInterface (m_pSerial);
}

void CPicoGPUGadget::OnSuspend (void)
{
	if (m_pFunction)
	{
		m_pFunction->OnDisconnect ();
	}

	delete m_pSerial;
	m_pSerial = nullptr;

	for (auto &pEP : m_pSerialEP)
	{
		delete pEP;
		pEP = nullptr;
	}
	delete m_pFunctionEP;
	m_pFunctionEP = nullptr;
}

boolean CPicoGPUGadget::ReceiveBulk (void *pBuffer, size_t nLength)
{
	return m_pFunctionEP && m_pFunctionEP->Receive (pBuffer, nLength);
}

void CPicoGPUGadget::CancelBulk (void)
{
	if (m_pFunctionEP)
	{
		m_pFunctionEP->Cancel ();
	}
}

// ---- the bulk OUT endpoint ------------------------------------------------------
//
// A host transfer of n bytes ends with a short packet, or after n bytes when n
// is a multiple of 512 (no zero-length packet follows). It's taken in chunks
// of at most MAX_CHUNK, each a multiple of 512 except the last.

CPicoGPUGadget::CBulkOutEndpoint::CBulkOutEndpoint (const TUSBEndpointDescriptor *pDesc,
						     CPicoGPUGadget *pGadget)
:	CDWUSBGadgetEndpoint (pDesc, pGadget),
	m_pGadget (pGadget),
	m_pBuffer (nullptr),
	m_nLength (0),
	m_nDone (0),
	m_nChunk (0),
	m_bActive (FALSE)
{
}

boolean CPicoGPUGadget::CBulkOutEndpoint::Receive (void *pBuffer, size_t nLength)
{
	if (m_bActive || !nLength)
	{
		return FALSE;
	}
	m_pBuffer = (u8 *) pBuffer;
	m_nLength = nLength;
	m_nDone = 0;
	m_bActive = TRUE;
	m_nChunk = nLength < MAX_CHUNK ? nLength : MAX_CHUNK;
	BeginTransfer (TransferDataOut, m_pBuffer, m_nChunk);

	return TRUE;
}

void CPicoGPUGadget::CBulkOutEndpoint::Cancel (void)
{
	if (m_bActive)
	{
		m_bActive = FALSE;
		CancelTransfer ();
	}
}

void CPicoGPUGadget::CBulkOutEndpoint::OnDeactivate (void)
{
	Cancel ();
}

void CPicoGPUGadget::CBulkOutEndpoint::OnTransferComplete (boolean bIn, size_t nLength)
{
	if (!m_bActive)
	{
		return;
	}
	m_nDone += nLength;
	if (nLength == m_nChunk && m_nDone < m_nLength)		// more to come
	{
		m_nChunk = m_nLength - m_nDone < MAX_CHUNK ? m_nLength - m_nDone : MAX_CHUNK;
		BeginTransfer (TransferDataOut, m_pBuffer + m_nDone, m_nChunk);
		return;
	}
	m_bActive = FALSE;
	if (m_pGadget->m_pFunction)
	{
		m_pGadget->m_pFunction->OnBulkReceived (m_nDone);
	}
}

//
// pgpugadget.h
//
// The Zero's USB device for the gpu app: one composite device with
//  - a serial port (CDC ACM, interfaces 0 and 1: devlink's log, the installer,
//    the PC's GL stream), as Circle's CUSBCDCGadget has it;
//  - a vendor interface (2) with a bulk OUT endpoint, for a display that the
//    Linux kernel drives as a monitor (GUD, drivers/gpu/drm/gud: gpu/display/
//    gud_display). Its control requests go to a CGadgetFunction.
// USB ID 1d50:614d: the one Linux' GUD driver binds to (with interface class
// 0xFF); cdc_acm binds to the serial port by its class.
//
#ifndef _devtools_pgpugadget_h
#define _devtools_pgpugadget_h

#include <circle/usb/gadget/dwusbgadget.h>
#include <circle/usb/gadget/dwusbgadgetendpoint.h>
#include <circle/usb/gadget/usbcdcgadget.h>
#include <circle/usb/gadget/usbcdcgadgetendpoint.h>
#include <circle/usb/usbserial.h>
#include <circle/interrupt.h>
#include <circle/macros.h>
#include <circle/types.h>

#define PGPU_GADGET_VENDOR_ID		0x1d50		// GUD's (Linux gud_drv.c)
#define PGPU_GADGET_PRODUCT_ID		0x614d
#define PGPU_GADGET_FUNCTION_INTERFACE	2

// the vendor interface's owner. Called from the USB interrupt.
class CGadgetFunction
{
public:
	virtual ~CGadgetFunction (void) {}

	// a vendor request to the interface. Device-to-host: the bytes put in
	// pData (at most nBufferSize), < 0 to stall. Host-to-device: after its
	// data (in pData, wLength bytes), < 0 to stall the status stage.
	virtual int OnVendorRequest (const TSetupData *pSetup, u8 *pData, size_t nBufferSize) = 0;

	// the transfer that ReceiveBulk started is done: nLength bytes
	virtual void OnBulkReceived (size_t nLength) = 0;

	// the host went away (unplugged, reset or suspended)
	virtual void OnDisconnect (void) = 0;
};

class CPicoGPUGadget : public CDWUSBGadget
{
public:
	CPicoGPUGadget (CInterruptSystem *pInterrupt);
	~CPicoGPUGadget (void);

	void SetFunction (CGadgetFunction *pFunction)	{ m_pFunction = pFunction; }

	const char *GetSerialNumber (void) const	{ return m_SerialNumber; }

	// receive nLength bytes on the bulk OUT endpoint into pBuffer (DMA-able,
	// cache-line aligned); FALSE if the host isn't connected or one is running
	boolean ReceiveBulk (void *pBuffer, size_t nLength);
	void CancelBulk (void);

protected:
	const void *GetDescriptor (u16 wValue, u16 wIndex, size_t *pLength) override;
	int OnClassOrVendorRequest (const TSetupData *pSetupData, u8 *pData) override;

private:
	void AddEndpoints (void) override;
	void CreateDevice (void) override;
	void OnSuspend (void) override;

	const void *ToStringDescriptor (const char *pString, size_t *pLength);

private:
	class CBulkOutEndpoint : public CDWUSBGadgetEndpoint
	{
	public:
		CBulkOutEndpoint (const TUSBEndpointDescriptor *pDesc, CPicoGPUGadget *pGadget);

		boolean Receive (void *pBuffer, size_t nLength);
		void Cancel (void);

		void OnActivate (void) override {}
		void OnDeactivate (void) override;
		void OnTransferComplete (boolean bIn, size_t nLength) override;

	private:
		CPicoGPUGadget *m_pGadget;
		u8 *m_pBuffer;
		size_t m_nLength;		// asked for
		size_t m_nDone;			// received so far
		size_t m_nChunk;		// the transfer running
		volatile boolean m_bActive;
	};
	friend class CBulkOutEndpoint;

	enum TEPNumber
	{
		EPNotif = 1,
		EPSerialOut = 2,
		EPSerialIn = 3,
		EPFunctionOut = 4,
		NumEPs
	};

	CGadgetFunction *m_pFunction;
	CUSBSerialDevice *m_pSerial;
	CUSBCDCGadgetEndpoint *m_pSerialEP[2];		// out, in
	CBulkOutEndpoint *m_pFunctionEP;

	char m_SerialNumber[20];
	u8 m_StringDescriptorBuffer[80];

	struct TInterfaceAssociationDescriptor
	{
		u8 bLength;
		u8 bDescriptorType;
		u8 bFirstInterface;
		u8 bInterfaceCount;
		u8 bFunctionClass;
		u8 bFunctionSubClass;
		u8 bFunctionProtocol;
		u8 iFunction;
	}
	PACKED;

	struct TConfigurationDescriptor
	{
		TUSBConfigurationDescriptor Configuration;
		TInterfaceAssociationDescriptor SerialAssociation;
		TUSBInterfaceDescriptor SerialControl;
		TUSBCDCACMInterfaceDescriptor SerialFunctional;
		TUSBEndpointDescriptor SerialNotify;
		TUSBInterfaceDescriptor SerialData;
		TUSBEndpointDescriptor SerialOut;
		TUSBEndpointDescriptor SerialIn;
		TUSBInterfaceDescriptor Function;
		TUSBEndpointDescriptor FunctionOut;
	}
	PACKED;

	static const TUSBDeviceDescriptor s_DeviceDescriptor;
	static const TConfigurationDescriptor s_ConfigurationDescriptor;
	static const char *const s_StringDescriptor[];
};

#endif

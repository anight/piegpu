//
// devlink.h
//
// Development link over the Pi Zero's USB port (USB serial CDC gadget):
//  - the log goes to the host as /dev/ttyACMx (the boot log is replayed on connect)
//  - the host sends DEVLINK_REBOOT_MAGIC to reboot into USB boot (rpiboot)
//  - a hardware watchdog reboots the board if Update() isn't called for a while
//    (hang or crash), so the host can always load the next build
//
#ifndef _devtools_devlink_h
#define _devtools_devlink_h

#include <circle/interrupt.h>
#include <circle/usb/gadget/usbcdcgadget.h>
#include <circle/usb/usbserial.h>
#include <circle/bcmwatchdog.h>
#include <circle/device.h>
#include <circle/types.h>

#define DEVLINK_REBOOT_MAGIC		"pico-gpu-reboot"
#define DEVLINK_WATCHDOG_SECONDS	10	// max. time between Update() calls

// pid.codes test VID/PID (for development only)
#define DEVLINK_USB_VENDOR_ID		0x1209
#define DEVLINK_USB_PRODUCT_ID		0x0001

class CDevLink
{
public:
	CDevLink (CInterruptSystem *pInterrupt);
	~CDevLink (void);

	/// \brief Starts the watchdog and the USB gadget
	/// \note Call after the interrupt system and the timer have been initialized.
	boolean Initialize (void);

	/// \brief Must be called regularly from the main loop
	void Update (void);

	/// \return Next character received from the host, or -1 if none
	int GetChar (void);

	/// \brief Write raw data to the host (not through the logger), waits
	/// until all is queued (1 s timeout)
	/// \return FALSE if there is no host or on timeout
	boolean Write (const void *pData, unsigned nLength);

private:
	void CheckMagic (const char *pData, unsigned nLength);

	static void DeviceRemovedHandler (CDevice *pDevice, void *pContext);

private:
	CUSBCDCGadget m_Gadget;
	CBcmWatchdog m_Watchdog;

	CUSBSerialDevice * volatile m_pSerial;
	CDevice *m_pPrevLogTarget;

	boolean m_bHostActive;		// host has sent data, so it is listening
	boolean m_bReplayDone;		// boot log has been replayed to the host

	const char *m_pMagicPtr;

	static const unsigned RxBufferSize = 64;
	char m_RxBuffer[RxBufferSize];
	unsigned m_nRxIn;
	unsigned m_nRxOut;
};

#endif

//
// devlink.h
//
// Development link over the RPi's USB port (USB serial CDC gadget):
//  - the log goes to the host as /dev/ttyACMx (the boot log is replayed on connect)
//  - the host sends DEVLINK_REBOOT_MAGIC to reboot into USB boot (rpiboot)
//  - a hardware watchdog reboots the board if Update() isn't called for a while
//    (hang or crash), so the host can always load the next build
//  - the host sends DEVLINK_STREAM_MAGIC to switch to a binary stream (until
//    the next reboot; the magic again starts a new session): the device
//    answers DEVLINK_STREAM_ACK, and all later data goes to StreamRead (); the
//    log goes on (the host tells it from its packets by their CRC)
//
#ifndef _devtools_devlink_h
#define _devtools_devlink_h

#include <circle/interrupt.h>
#include <circle/usb/gadget/usbcdcgadget.h>
#include <circle/usb/usbserial.h>
#include <circle/bcmwatchdog.h>
#include <circle/device.h>
#include <circle/types.h>

#define DEVLINK_REBOOT_MAGIC		"piegpu-reboot"
#define DEVLINK_STREAM_MAGIC		"piegpu-stream"
#define DEVLINK_STREAM_ACK		"\n#STREAM\n"
#define DEVLINK_WATCHDOG_SECONDS	10	// max. time between Update() calls

// pid.codes test VID/PID (for development only)
#define DEVLINK_USB_VENDOR_ID		0x1209
#define DEVLINK_USB_PRODUCT_ID		0x0001

class CDevLink
{
public:
	/// \param pGadget The USB device to use (a composite one that has a CDC
	/// serial port: its CUSBSerialDevice becomes "utty1"), or nullptr for a
	/// plain CDC serial gadget (DEVLINK_USB_VENDOR_ID, _PRODUCT_ID)
	CDevLink (CInterruptSystem *pInterrupt, CDWUSBGadget *pGadget = nullptr);
	~CDevLink (void);

	/// \brief Starts the watchdog and the USB gadget
	/// \note Call after the interrupt system and the timer have been initialized.
	boolean Initialize (void);

	/// \brief Must be called regularly from the main loop
	void Update (void);

	/// \return Next character received from the host, or -1 if none
	int GetChar (void);

	/// \return TRUE after the host has switched to the binary stream
	boolean IsStreaming (void) const	{ return m_bStream; }

	/// \brief Read from the binary stream
	/// \return Number of bytes read (0 if none)
	unsigned StreamRead (void *pBuffer, unsigned nMax);

	/// \return Bytes of the stream taken from the USB gadget so far (the
	/// gadget has no flow control: the host needs to know)
	u32 GetStreamReceived (void) const	{ return m_nStreamReceived; }

	/// \brief Called before the reboot the host asks for (DEVLINK_REBOOT_MAGIC)
	void RegisterRebootHandler (void (*pHandler) (void))	{ m_pRebootHandler = pHandler; }

	/// \brief Write raw data to the host (not through the logger), waits
	/// until all is queued (1 s timeout)
	/// \return FALSE if there is no host or on timeout
	boolean Write (const void *pData, unsigned nLength);

private:
	void Receive (const char *pData, unsigned nLength);
	unsigned ScanMagic (const char *pData, unsigned nLength);
	void StartStream (void);

	static void DeviceRemovedHandler (CDevice *pDevice, void *pContext);

private:
	CDWUSBGadget *m_pGadget;
	CBcmWatchdog m_Watchdog;

	CUSBSerialDevice * volatile m_pSerial;
	CDevice *m_pPrevLogTarget;
	void (*m_pRebootHandler) (void);

	boolean m_bHostActive;		// host has sent data, so it is listening
	boolean m_bReplayDone;		// boot log has been replayed to the host

	const char *m_pMagicPtr;
	const char *m_pStreamMagicPtr;

	boolean m_bStream;
	static const unsigned StreamSize = 512 * 1024;
	u8 *m_pStream;			// ring
	unsigned m_nStreamIn;
	unsigned m_nStreamOut;
	u32 m_nStreamReceived;

	static const unsigned RxBufferSize = 20 * 1024;	// a line of the installer (gpu/install)
	char m_RxBuffer[RxBufferSize];
	unsigned m_nRxIn;
	unsigned m_nRxOut;
};

#endif

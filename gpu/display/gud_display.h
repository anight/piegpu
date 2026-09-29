//
// gud_display.h
//
// The RPi as a monitor for a Linux PC, over its USB port: the device side
// of GUD, the Linux kernel's Generic USB Display (drivers/gpu/drm/gud; the
// protocol: include/drm/gud.h). The PC's desktop shows on the screen the RPi
// has now (the panel or HDMI) while the PC has the display enabled; the GL
// host's frames are rendered off screen meanwhile (the kernel's choice).
//
// One connector, connected, with one mode: the screen's size (it changes
// with the screen: the PC polls every 10 s and takes the new mode). Pixels:
// RGB565 (the PC converts XRGB8888), no compression. The PC sends each
// changed rectangle in bands of at most BandBytes: SET_BUFFER, then a bulk
// transfer into one of two receive buffers; the main loop copies it into the
// picture. With both buffers full, the next transfer starts only when one is
// free (the PC's bulk transfer waits, up to 3 s).
//
// Control requests come in the USB interrupt; errors are reported through
// GET_STATUS (flag STATUS_ON_SET: the gadget can't stall a SET's status stage).
//
#ifndef _gud_display_h
#define _gud_display_h

#include "output.h"
#include <pgpugadget.h>
#include <circle/spinlock.h>
#include <circle/types.h>

class CGUDDisplay : public CGadgetFunction
{
public:
	static const unsigned MinWidth = 320, MinHeight = 240;
	static const unsigned MaxWidth = 1920, MaxHeight = 1200;
	static const unsigned BandBytes = 256 * 1024;

	CGUDDisplay (CPicoGPUGadget *pGadget);
	~CGUDDisplay (void);

	boolean Initialize (void);

	// the screen's size now (the connector's mode)
	void SetScreen (unsigned nWidth, unsigned nHeight);

	// the PC has the display on, in the screen's size
	boolean IsActive (void) const;

	// main loop: takes received bands into the picture, and shows it on
	// pOutput (the screen, while active); TRUE if the active state changed
	boolean Update (COutput *pOutput);

	// the desktop starts on this output: the picture drawn anew on it
	void Start (COutput *pOutput);

	int OnVendorRequest (const TSetupData *pSetup, u8 *pData, size_t nBufferSize) override;
	void OnBulkReceived (size_t nLength) override;
	void OnDisconnect (void) override;

private:
	struct TBand
	{
		unsigned x, y, nWidth, nHeight;
		u8 *pData;
		volatile boolean bFull;		// received, not copied yet
	};

	int GetRequest (u8 bRequest, u16 wValue, u8 *pData, size_t nBufferSize);
	u8 SetRequest (u8 bRequest, u16 wValue, const u8 *pData, size_t nLength);
	u8 SetBuffer (const u8 *pData, size_t nLength);
	boolean StartReceive (void);		// the pending band, if a buffer is free

	void Draw (const TBand &rBand);
	void ShowPicture (COutput *pOutput);

	CPicoGPUGadget *m_pGadget;
	CSpinLock m_SpinLock;

	volatile unsigned m_nScreenWidth, m_nScreenHeight;
	volatile boolean m_bChanged;		// the connector's mode changed: tell the PC

	// the PC's state
	volatile u8 m_uchStatus;		// of the last request (GET_STATUS)
	volatile unsigned m_nCheckedWidth, m_nCheckedHeight;	// STATE_CHECK
	volatile unsigned m_nWidth, m_nHeight;			// STATE_COMMIT
	volatile boolean m_bControllerOn;
	volatile boolean m_bDisplayOn;

	// transfers
	TBand m_Band[2];
	u8 *m_pBandMemory;
	volatile int m_nReceiving;		// the band being received (-1: none)
	volatile boolean m_bPending;		// a SET_BUFFER waits for a free buffer
	TBand m_PendingBand;
	volatile unsigned m_nNextBand;

	// the picture (RGB565, the screen's size): our own, or the output's page
	u16 *m_pOwnPicture;
	u16 *m_pPicture;
	unsigned m_nPictureWidth, m_nPictureHeight;
	COutput *m_pShownOn;
	boolean m_bDirty;
	boolean m_bWasActive;
	unsigned m_nUpdates;			// bands drawn (statistics, a second)
	unsigned m_nBytes;
	unsigned m_nLastReport;
	volatile unsigned m_nProbeTicks;	// the PC's GET_DESCRIPTOR (0: none yet)
};

#endif

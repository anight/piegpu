//
// hdmi_monitor.h
//
// Watches the mini-HDMI connector: the hot-plug line (HPD, low while a
// monitor is connected: GPIO46 on a Zero, GPIO28 on a Zero 2 W - read only,
// the firmware owns the pin) and, when a
// monitor appears, its EDID, read over the DDC bus (BSC2, I2C address 0x50)
// directly. The firmware's EDID property tag can't be used: it keeps
// answering with the EDID read at boot after the monitor is gone.
//
// The firmware doesn't change the HDMI mode by itself after boot (config.txt
// has hdmi_force_hotplug=1, so without a monitor at boot it's 640x480), and
// at boot it takes a mode of its lists, which a monitor's own may not be in
// (1024x600: it sent 1024x768). The mode being sent is read from the HDMI
// pixel valve (PV2); the monitor's own timing is kept here for the kernel to
// ask the firmware for it (CKernel::MatchHDMIMode, tv_service.h).
//
#ifndef _hdmi_monitor_h
#define _hdmi_monitor_h

#include <circle/types.h>

struct THDMIState
{
	boolean bConnected;
	boolean bEDID;			// the EDID was read (the fields below are from it)
	unsigned nWidth, nHeight;	// the monitor's preferred mode (0: unknown)
	unsigned nRefreshMilliHz;
	char Name[14];			// the monitor's name, if it has one
	// the preferred mode's timing (the EDID's first detailed one)
	unsigned nClockHz;
	unsigned nHFront, nHSync, nHBack;
	unsigned nVFront, nVSync, nVBack;
	boolean bHSyncPositive, bVSyncPositive, bInterlaced;
	boolean bAudio;			// it takes sound (a CEA extension that says so): HDMI, not DVI
	unsigned nSignalWidth;		// the mode sent (0: HDMI not active)
	unsigned nSignalHeight;
};

class CHDMIMonitor
{
public:
	CHDMIMonitor (void);

	/// \brief Look at the connector now (at boot; waits for the EDID)
	void Initialize (void);

	/// \brief Call often: samples the hot-plug line every few milliseconds
	/// \return TRUE if the state has changed
	boolean Update (void);

	const THDMIState &GetState (void) const	{ return m_State; }

	/// \brief The mode sent may have changed: read it again
	/// \param pTimings if not nullptr: the pixel valve's HORZA, HORZB, VERTA, VERTB
	void SignalChanged (u32 *pTimings = nullptr);

private:
	boolean ReadHPD (void);
	boolean ReadEDID (u8 *pBlock, unsigned nOffset = 0);
	void Connected (boolean bNow);
	boolean ParseEDID (const u8 *pBlock);
	void ReadSignal (void);

private:
	THDMIState m_State;

	unsigned m_nHPDPin;
	boolean m_bLastSample;
	unsigned m_nSameSamples;
	unsigned m_nLastSampleTicks;
	unsigned m_nLastEDIDTicks;		// connected without EDID: tried again every second
};

#endif

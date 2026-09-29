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
// The firmware doesn't change the HDMI mode after boot (config.txt has
// hdmi_force_hotplug=1, so without a monitor at boot it's 640x480): the mode
// being sent is read from the HDMI pixel valve (PV2).
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

private:
	boolean ReadHPD (void);
	boolean ReadEDID (u8 *pBlock);
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

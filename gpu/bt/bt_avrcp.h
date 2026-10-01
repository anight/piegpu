//
// bt_avrcp.h
//
// The speaker's volume and ours as one, over the remote control channel
// (AVCTP, L2CAP PSM 0x17): we set its volume (AVRCP's absolute volume, 0 ..
// 127) and it tells us when its own buttons change it. A speaker without
// that may send its volume buttons' presses instead (pass through), which
// are counted; its other commands (play, pause, what it asks of a player)
// are answered and nothing more.
//
// AVCTP 1.4 (single packets only); AVRCP 1.4 (6.13: volume handling).
//
#ifndef _gpu_bt_bt_avrcp_h
#define _gpu_bt_bt_avrcp_h

#include <circle/types.h>

class CBTAVRCPHost
{
public:
	virtual ~CBTAVRCPHost (void) {}
	virtual boolean SendControl (const u8 *pData, unsigned nBytes) = 0;
};

class CBTAVRCP
{
public:
	static const unsigned MaxVolume = 127;

	CBTAVRCP (CBTAVRCPHost *pHost);

	void ChannelOpen (void);
	void ChannelClosed (void);
	void OnData (const u8 *pData, unsigned nBytes);
	/// \brief Call often: the answers that never came, what waits to be sent
	void Update (void);

	/// \return TRUE while the speaker's volume is set from here
	boolean HasVolume (void) const		{ return m_bVolume; }
	/// \brief The speaker's volume (0 .. MaxVolume), as soon as it takes one
	void SetVolume (unsigned nVolume);
	/// \return TRUE once after the speaker's own buttons changed its volume
	boolean VolumeChanged (unsigned *pVolume);
	/// \return The volume buttons' presses since the last call (up: +1, down: -1 each),
	///	    from a speaker that leaves the volume to us
	int GetButtons (void);

	/// \brief Log every frame of the channel (the btlog=on option)
	void SetTrace (boolean bTrace)		{ m_bTrace = bTrace; }
	/// \brief Debugging: one of AVRCP's commands by hand (its answer is logged)
	void Probe (u8 nType, u8 nPDU, const u8 *pParams, unsigned nBytes);

private:
	void Vendor (u8 nType, u8 nPDU, const u8 *pParams, unsigned nBytes);
	void Answer (const u8 *pFrame, unsigned nBytes, u8 nType, const u8 *pParams, unsigned nParams);
	void OnCommand (const u8 *pFrame, unsigned nBytes);
	void OnAnswer (const u8 *pFrame, unsigned nBytes);
	void Register (void);

private:
	CBTAVRCPHost *m_pHost;
	boolean m_bOpen;
	boolean m_bVolume;			// it takes a volume
	boolean m_bAsked;			// its events are asked for
	u8 m_nLabel;				// the next command's
	unsigned m_nSpeaker;			// its volume as last known (~0: not)
	unsigned m_nWanted;			// the one to set (~0: none)
	unsigned m_nSent;			// the one last sent (~0: none)
	boolean m_bSetting;			// a volume is on its way
	unsigned m_nSetAt;			// when it was sent
	boolean m_bChanged;			// by its buttons, not told yet
	int m_nButtons;
	unsigned m_nOpenAt;
	boolean m_bTrace;
};

#endif

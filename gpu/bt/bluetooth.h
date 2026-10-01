//
// bluetooth.h
//
// Bluetooth, for a speaker: the board's controller brought up (bt_uart,
// bt_hci), the audio devices nearby found (an inquiry, their names), one of
// them connected and paired with, the sound sent to it (A2DP: bt_l2cap,
// bt_avdtp, bt_audio), and its volume kept the same as ours (AVRCP:
// bt_avrcp). Only in the kernels of the boards that have the chip
// (PGPU_WIRELESS: the Zero W's, the Zero 2 W's).
//
// All of it runs in the main loop (Update); only the UART's bytes move in an
// interrupt.
//
#ifndef _gpu_bt_bluetooth_h
#define _gpu_bt_bluetooth_h

#include "bt_hci.h"
#include "bt_l2cap.h"
#include "bt_avdtp.h"
#include "bt_audio.h"
#include "bt_avrcp.h"
#include <circle/interrupt.h>
#include <circle/types.h>

class CBluetooth : public CBTHCIClient, public CBTL2CAPClient, public CBTAVDTPHost, public CBTAudioHost,
		   public CBTAVRCPHost
{
public:
	static const unsigned MaxDevices = 12;
	static const unsigned MaxBonds = 8;
	static const unsigned NameChars = 48;

	struct TDevice
	{
		u8 Address[6];			// (as on the air: the low byte first)
		char Name[NameChars];		// "" till it's known
		u32 nClass;
		s8 nRSSI;			// dBm (0: not given)
		u8 nPageScanMode;
		u16 nClockOffset;
		boolean bAudio;			// a speaker, headphones: something to send sound to
		boolean bNameAsked;
		unsigned nSeen;			// the scan it was last seen in
	};

	enum TState
	{
		StateOff,
		StateStarting,			// the controller is being set up
		StateReady,
		StateFailed			// the controller doesn't answer
	};

	enum TLink				// the speaker's connection
	{
		LinkNone,
		LinkConnecting,			// it's being called
		LinkPairing,			// connected: the two prove (or make) their key
		LinkSetup,			// the stream is being set up
		LinkReady,			// the sound can go there
		LinkClosing
	};

	struct TBond				// a speaker paired with
	{
		u8 Address[6];
		u8 Key[16];
		u8 nKeyType;
		char Name[NameChars];
	};

	CBluetooth (CInterruptSystem *pInterrupt);

	/// \brief Log every packet (the btlog=on option)
	void SetTrace (boolean bTrace)		{ m_HCI.SetTrace (bTrace); m_AVRCP.SetTrace (bTrace); }
	/// \brief Debugging: an AVRCP command to the speaker by hand
	void ControlProbe (u8 nType, u8 nPDU, const u8 *pParams, unsigned nBytes)	{ m_AVRCP.Probe (nType, nPDU, pParams, nBytes); }

	/// \brief On: the controller set up (it takes a moment: GetState); off: reset, quiet
	void SetEnabled (boolean bOn);
	boolean IsEnabled (void) const		{ return m_State != StateOff; }
	TState GetState (void) const		{ return m_State; }

	/// \brief Call often (the main loop)
	void Update (void);

	/// \brief Look for devices, again and again, while on (and nothing is connected)
	void SetScanning (boolean bOn);
	boolean IsScanning (void) const		{ return m_bScanning; }
	/// \return The audio devices found, the nearest first; a number that changes when they do
	unsigned GetDevices (const TDevice **ppDevices) const;
	unsigned GetGeneration (void) const	{ return m_nGeneration; }

	/// \brief Connect to a device (found, or the one paired with), pair with it if
	///	   it's new, set up its stream. Another one connected is let go first
	void Connect (const u8 *pAddress);
	/// \brief Let the connected device go (it stays paired with)
	void Disconnect (void);
	TLink GetLink (void) const		{ return m_Link; }
	/// \return The device connected to (or being connected to)
	const u8 *GetLinkAddress (void) const	{ return m_LinkAddress; }
	/// \return Why the last connection ended or failed, "" if it didn't
	const char *GetLinkError (void) const	{ return m_LinkError; }

	/// \brief The speakers paired with (kept on the card), the last used first.
	///	   While none is connected they're called in turn, and their calls taken
	void SetBonds (const TBond *pBonds, unsigned nBonds);
	unsigned GetBonds (const TBond **ppBonds) const	{ *ppBonds = m_Bond; return m_nBonds; }
	const TBond *FindBond (const u8 *pAddress) const;
	/// \return TRUE once after they changed (they're to be saved)
	boolean BondChanged (void);
	/// \brief A speaker is paired with no more (let go, if it's the connected one)
	void Forget (const u8 *pAddress);
	/// \return The name of a device paired with or found, or its address
	const char *GetName (const u8 *pAddress);

	/// \return The sound's output to the speaker (the audio's other sink)
	CBTAudioOut *GetAudioOut (void)		{ return &m_Audio; }

	/// \return TRUE while the connected speaker's own volume is set from here (the
	///	    sound then goes to it as it is: SetVolume is the volume)
	boolean HasVolume (void) const		{ return m_AVRCP.HasVolume (); }
	void SetVolume (unsigned nPercent);
	/// \return TRUE once after the speaker's buttons changed its volume
	boolean VolumeChanged (unsigned *pPercent);
	/// \return The volume buttons' presses of a speaker that leaves the volume to
	///	    us, since the last call (up: +1, down: -1 each)
	int GetVolumeButtons (void)		{ return m_AVRCP.GetButtons (); }

	static void FormatAddress (const u8 *pAddress, char *pText);	// "AA:BB:CC:DD:EE:FF" (18 bytes)
	static boolean ParseAddress (const char *pText, u8 *pAddress);

private:
	// CBTHCIClient
	void OnEvent (u8 nCode, const u8 *pParams, unsigned nBytes) override;
	void OnFrame (u16 nHandle, const u8 *pFrame, unsigned nBytes) override;
	// CBTL2CAPClient
	boolean OnChannelAsked (u16 nPSM) override;
	void OnChannelOpen (unsigned nChannel, u16 nPSM, boolean bTheirs) override;
	void OnChannelData (unsigned nChannel, u16 nPSM, const u8 *pData, unsigned nBytes) override;
	void OnChannelClosed (unsigned nChannel, u16 nPSM) override;
	// CBTAVDTPHost
	boolean SendSignal (const u8 *pData, unsigned nBytes) override;
	void OpenMedia (void) override;
	void CloseMedia (void) override;
	void OnStream (void) override;
	// CBTAudioHost
	unsigned GetMediaMTU (void) override;
	boolean SendMedia (const u8 *pPacket, unsigned nBytes) override;
	// CBTAVRCPHost
	boolean SendControl (const u8 *pData, unsigned nBytes) override;

	void Call (void);			// the connection asked for
	void LinkEvent (u8 nCode, const u8 *pParams, unsigned nBytes);
	void SetLink (TLink Link);
	void LinkLost (const char *pWhy);
	void HangUp (const char *pWhy);
	static const char *StatusText (u8 nStatus);
	void Remember (const TBond &Bond);	// (to the list's front)

	void Step (void);			// the set-up's next command
	void CommandDone (u16 nOpcode, const u8 *pReturn, unsigned nBytes);
	void Fail (const char *pWhy);

	void Inquire (void);
	void InquiryDone (void);
	void Found (const u8 *pAddress, u8 nPageScanMode, const u8 *pClass, u16 nClockOffset, int nRSSI,
		    const u8 *pEIR);
	TDevice *Find (const u8 *pAddress);
	void AskName (void);

private:
	CBTUart m_Uart;
	CBTHCI m_HCI;
	TState m_State;
	unsigned m_nStep;			// of the set-up
	unsigned m_nStepStart;			// CTimer::GetClockTicks ()
	unsigned m_nTries;
	u8 m_Address[6];

	boolean m_bScanning;
	boolean m_bInquiring, m_bNaming;
	unsigned m_nScan;			// scans so far
	unsigned m_nScanIdle;			// when the last one ended
	TDevice m_Device[MaxDevices];
	unsigned m_nDevices;
	TDevice m_Sorted[MaxDevices];
	unsigned m_nGeneration;

	// the connection
	CBTL2CAP m_L2CAP;
	CBTAVDTP m_AVDTP;
	CBTAudioOut m_Audio;
	CBTAVRCP m_AVRCP;
	TLink m_Link;
	unsigned m_nLinkSince;			// the state's start
	u8 m_LinkAddress[6];
	u16 m_nHandle;
	boolean m_bTheirCall;			// they connected to us
	boolean m_bKeyUsed, m_bKeyRefused;	// the stored key was given; the device didn't take it
	boolean m_bEncrypted;
	unsigned m_nSignalling, m_nMedia;	// the stream's channels (CBTL2CAP::None: not there)
	unsigned m_nControl;			// the remote control's channel
	boolean m_bControlTried;		// it was asked for on this connection
	boolean m_bPending;			// a connection to m_Pending once this one is gone
	u8 m_Pending[6];
	char m_LinkError[48];
	TBond m_Bond[MaxBonds];
	unsigned m_nBonds;
	boolean m_bBondChanged;
	unsigned m_nLastCall;			// a paired speaker's last call
	unsigned m_nCallNext;			// whose turn it is
	char m_NameText[NameChars];
	boolean m_bUserHangUp;			// let go by hand: not called again by itself
};

#endif

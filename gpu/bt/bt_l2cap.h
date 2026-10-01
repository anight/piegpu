//
// bt_l2cap.h
//
// L2CAP over the one connection (the speaker's): channels, each to a service
// of the other side (its PSM), opened by us or by it: the signalling (asking,
// answering, each side saying what frames it takes) and the frames. Basic
// mode only (what A2DP uses): no retransmission, no segmentation.
//
// Bluetooth Core specification, vol 3 part A.
//
#ifndef _gpu_bt_bt_l2cap_h
#define _gpu_bt_bt_l2cap_h

#include "bt_hci.h"
#include <circle/types.h>

#define PSM_SDP			0x0001
#define PSM_AVCTP		0x0017
#define PSM_AVDTP		0x0019

class CBTL2CAPClient
{
public:
	virtual ~CBTL2CAPClient (void) {}
	/// \return TRUE to take a channel the other side asks for
	virtual boolean OnChannelAsked (u16 nPSM) = 0;
	/// \brief A channel is open (ours, or one we took): frames may go
	virtual void OnChannelOpen (unsigned nChannel, u16 nPSM, boolean bTheirs) = 0;
	virtual void OnChannelData (unsigned nChannel, u16 nPSM, const u8 *pData, unsigned nBytes) = 0;
	/// \brief A channel is gone (closed, refused, or the connection lost)
	virtual void OnChannelClosed (unsigned nChannel, u16 nPSM) = 0;
};

class CBTL2CAP
{
public:
	static const unsigned Channels = 6;
	static const unsigned None = ~0U;
	static const unsigned OurMTU = 1017;		// (a frame of it fits the controller's packet)

	CBTL2CAP (CBTHCI *pHCI, CBTL2CAPClient *pClient);

	void LinkUp (u16 nHandle);
	void LinkDown (void);				// (every channel closed)
	boolean IsLinkUp (void) const			{ return m_bLink; }

	/// \brief A frame of the connection's (CBTHCIClient::OnFrame)
	void OnFrame (const u8 *pFrame, unsigned nBytes);
	/// \brief Call often: the answers that never came
	void Update (void);

	/// \return The channel being opened (OnChannelOpen or OnChannelClosed follows), or None
	unsigned Open (u16 nPSM);
	void Close (unsigned nChannel);
	boolean IsOpen (unsigned nChannel) const;
	/// \return The longest payload the other side takes on it
	unsigned GetMTU (unsigned nChannel) const;
	/// \return FALSE if it can't be queued now (dropped)
	boolean Send (unsigned nChannel, const u8 *pData, unsigned nBytes);

private:
	enum TState { Free, Asking, Configuring, Opened, Closing };
	struct TChannel
	{
		TState State;
		u16 nPSM;
		u16 nTheirCID;
		boolean bTheirs;			// they asked for it
		boolean bOursDone, bTheirsDone;		// each side's configuration agreed
		unsigned nMTU;				// what they take
		unsigned nSince;			// the state's start
	};

	void Signal (u8 nCode, u8 nId, const u8 *pData, unsigned nBytes);
	void OnSignal (u8 nCode, u8 nId, const u8 *pData, unsigned nBytes);
	void Configure (unsigned nChannel);
	void Check (unsigned nChannel);
	void Drop (unsigned nChannel);
	static u16 OurCID (unsigned nChannel)		{ return 0x40 + nChannel; }
	unsigned ByOurCID (u16 nCID) const;

private:
	CBTHCI *m_pHCI;
	CBTL2CAPClient *m_pClient;
	boolean m_bLink;
	u16 m_nHandle;
	u8 m_nId;
	TChannel m_Channel[Channels];
};

#endif

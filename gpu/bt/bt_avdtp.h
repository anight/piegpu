//
// bt_avdtp.h
//
// The stream to the speaker, set up over AVDTP's signalling channel: its
// stream end points asked for, the one that takes SBC chosen and configured
// (the rate the sound has, joint stereo, 16 blocks, 8 subbands, loudness, the
// highest bitpool it takes up to 53), opened (its transport channel), and
// started while there's sound to send. The speaker's own commands to our end
// point are answered too (some configure the stream themselves).
//
// AVDTP 1.3; A2DP 1.3 (the SBC codec's capabilities, 4.3).
//
#ifndef _gpu_bt_bt_avdtp_h
#define _gpu_bt_bt_avdtp_h

#include <circle/types.h>

class CBTAVDTPHost
{
public:
	virtual ~CBTAVDTPHost (void) {}
	virtual boolean SendSignal (const u8 *pData, unsigned nBytes) = 0;
	/// \brief Open the stream's transport channel (MediaOpen or MediaClosed follows)
	virtual void OpenMedia (void) = 0;
	virtual void CloseMedia (void) = 0;
	/// \brief The stream is configured anew, opened, started, stopped, gone
	virtual void OnStream (void) = 0;
};

class CBTAVDTP
{
public:
	struct TConfig					// the stream's SBC
	{
		unsigned nRate;				// 44100 or 48000
		unsigned nChannels;			// 2 (1: mono)
		boolean bJoint;				// joint stereo (else stereo)
		boolean bDual;				// dual channel
		unsigned nBlocks;			// 4, 8, 12, 16
		unsigned nSubbands;			// 4, 8
		boolean bSNR;				// the allocation (else loudness)
		unsigned nBitpool;
	};

	enum TState
	{
		Idle,					// no signalling channel
		Waiting,				// it's open: a moment for the speaker's own commands
		Discovering, GettingCaps, Configuring, Opening,
		WaitMedia,				// the transport channel is being opened
		Open,					// ready: not started
		Starting, Streaming, Suspending, Reconfiguring,
		Failed
	};

	CBTAVDTP (CBTAVDTPHost *pHost);

	void SignallingOpen (void);
	void SignallingClosed (void);
	void MediaOpen (void);
	void MediaClosed (void);
	void OnSignal (const u8 *pData, unsigned nBytes);
	void Update (void);

	/// \brief The rate the sound has (44100 or 48000): the stream's, if the speaker takes it
	void SetRate (unsigned nRate);
	/// \brief Sound to send (the stream started), or none (suspended)
	void SetWanted (boolean bWanted);

	TState GetState (void) const		{ return m_State; }
	boolean IsReady (void) const		{ return m_State >= Open && m_State <= Reconfiguring; }
	boolean IsStreaming (void) const	{ return m_State == Streaming; }
	const TConfig &GetConfig (void) const	{ return m_Config; }

private:
	void Command (u8 nSignal, const u8 *pData = nullptr, unsigned nBytes = 0);
	void Answer (u8 nLabel, u8 nSignal, boolean bAccept, const u8 *pData = nullptr, unsigned nBytes = 0);
	void OnAnswer (u8 nSignal, boolean bAccept, const u8 *pData, unsigned nBytes);
	void OnCommand (u8 nLabel, u8 nSignal, const u8 *pData, unsigned nBytes);
	void NextEndPoint (void);
	void Configure (boolean bAgain);
	boolean Choose (const u8 *pCaps, unsigned nRate, TConfig *pConfig, u8 *pBytes) const;
	static boolean Parse (const u8 *pBytes, TConfig *pConfig);
	static const u8 *FindSBC (const u8 *pCaps, unsigned nBytes);
	void Go (void);
	void Enter (TState State);
	void Fail (const char *pWhy);

private:
	CBTAVDTPHost *m_pHost;
	TState m_State;
	unsigned m_nSince;				// the state's start
	u8 m_nLabel;					// our last command's
	u8 m_nSignal;					// ... and its signal (0: none waits for its answer)

	static const unsigned MaxEndPoints = 8;
	u8 m_EndPoint[MaxEndPoints];			// their sinks' IDs
	unsigned m_nEndPoints, m_nEndPoint;
	u8 m_nTheirSEID;
	u8 m_TheirCaps[4];				// their SBC capabilities
	boolean m_bTheirsKnown;
	boolean m_bMedia;				// the transport channel is open

	TConfig m_Config;
	u8 m_ConfigBytes[4];
	TConfig m_NewConfig;				// being asked for
	u8 m_NewBytes[4];
	unsigned m_nRate;				// wanted
	boolean m_bWanted;
};

#endif

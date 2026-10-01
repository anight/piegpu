//
// bt_audio.h
//
// The sound to a Bluetooth speaker (an audio sink, audio/audio_sink.h): the
// ring's frames encoded as SBC (sbc_encoder/: Bluedroid's, Apache 2.0) and
// sent over the stream's transport channel as A2DP media packets, a few SBC
// frames each, at the pace they are played: the main loop (Update) sends what
// the time since the last call asks for. A speaker takes its stream at 44.1
// or at 48 kHz (bt_avdtp chooses the sound's rate when the speaker has it);
// sound of another rate is resampled (linear interpolation).
//
// The stream is started when there's sound, and suspended when there has been
// none for a while. The first frames wait for it to start (not lost); if it
// doesn't, they are dropped at their pace, so what plays the sound goes on.
//
#ifndef _gpu_bt_bt_audio_h
#define _gpu_bt_bt_audio_h

#include "bt_avdtp.h"
#include "../audio/audio.h"
#include <circle/types.h>

struct SBC_ENC_PARAMS_TAG;

class CBTAudioHost
{
public:
	virtual ~CBTAudioHost (void) {}
	/// \return The longest media packet the speaker takes (0: no channel)
	virtual unsigned GetMediaMTU (void) = 0;
	virtual boolean SendMedia (const u8 *pPacket, unsigned nBytes) = 0;
};

class CBTAudioOut : public CAudioSinkOther
{
public:
	CBTAudioOut (CBTAVDTP *pStream, CBTAudioHost *pHost);
	~CBTAudioOut (void);

	boolean SetSource (unsigned nRate) override;
	boolean Open (void) override;
	void Close (void) override		{}
	unsigned GetChunkFrames (void) const override;
	unsigned GetLatencyFrames (void) const override;

	/// \brief Call often (the main loop)
	void Update (void);
	/// \brief The stream changed (bt_avdtp): its configuration is the encoder's
	void OnStream (void);

	/// \return Media packets sent, and those dropped (no room on the way)
	unsigned GetSent (void) const		{ return m_nSent; }
	unsigned GetDropped (void) const	{ return m_nDropped; }
	/// \return The longest the main loop was away while the stream ran (us), since the last call;
	///	    how often it was away longer than what's made up for (that sound skipped)
	unsigned GetLongestGap (void)		{ unsigned n = m_nLongest; m_nLongest = 0; return n; }
	unsigned GetSkips (void) const		{ return m_nSkips; }

private:
	void Frame (boolean bSend);		// one SBC frame's worth of sound taken (and sent)
	void Fill (s16 *pFrames, unsigned nFrames);

private:
	static const unsigned InFrames = 64;	// taken from the ring at a time, when resampling

	CBTAVDTP *m_pStream;
	CBTAudioHost *m_pHost;

	struct SBC_ENC_PARAMS_TAG *m_pEncoder;
	boolean m_bEncoder;			// set up for the stream's configuration
	unsigned m_nOutRate;			// the stream's
	unsigned m_nFrameSamples;		// an SBC frame's (blocks x subbands)

	unsigned m_nLast;			// Update's last time
	u64 m_nDue;				// frames to send, x 1 000 000
	unsigned m_nLastSound;			// when the ring last had something
	unsigned m_nWanted;			// since when the stream is wanted
	boolean m_bWanted;

	// resampling: the two frames the output lies between, where between them (16.16)
	s16 m_Previous[2], m_Current[2];
	u32 m_nPhase;
	s16 m_In[InFrames * 2];
	unsigned m_nIn, m_nInAt;

	// the media packet being filled
	u8 m_Packet[13 + 15 * 160];
	unsigned m_nPacket;			// its bytes
	unsigned m_nPacketFrames;
	u16 m_nSequence;
	u32 m_nTimestamp;
	unsigned m_nSent, m_nDropped;
	unsigned m_nLongest, m_nSkips;
};

#endif

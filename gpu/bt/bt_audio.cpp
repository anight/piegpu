//
// bt_audio.cpp
//
#include "bt_audio.h"
#include <circle/logger.h>
#include <circle/timer.h>
#include <circle/util.h>

extern "C"
{
#include "sbc_encoder/include/sbc_encoder.h"
}

LOGMODULE ("btaudio");

#define IDLE_US			5000000		// no sound for so long: the stream suspended
#define START_US		3000000		// the stream not started after so long: the sound isn't held
#define LATENCY_MS		200		// a speaker's own buffer (not known: about this)
#define MAX_BEHIND_US		250000		// the main loop was away longer: that sound is skipped
#define PACKET_BYTES		660		// a media packet at most (one baseband packet carries it)

CBTAudioOut::CBTAudioOut (CBTAVDTP *pStream, CBTAudioHost *pHost)
:	CAudioSinkOther (44100),
	m_pStream (pStream),
	m_pHost (pHost),
	m_pEncoder (new SBC_ENC_PARAMS),
	m_bEncoder (FALSE),
	m_nOutRate (44100),
	m_nFrameSamples (128),
	m_nLast (0),
	m_nDue (0),
	m_nLastSound (0),
	m_nWanted (0),
	m_bWanted (FALSE),
	m_nPhase (0),
	m_nIn (0),
	m_nInAt (0),
	m_nPacket (0),
	m_nPacketFrames (0),
	m_nSequence (0),
	m_nTimestamp (0),
	m_nSent (0),
	m_nDropped (0),
	m_nLongest (0),
	m_nSkips (0)
{
	memset (m_Previous, 0, sizeof m_Previous);
	memset (m_Current, 0, sizeof m_Current);
}

CBTAudioOut::~CBTAudioOut (void)
{
	delete m_pEncoder;
}

// sound of this rate is to come: here, if the speaker's stream is there
boolean CBTAudioOut::SetSource (unsigned nRate)
{
	if (!m_pStream->IsReady ())
	{
		return FALSE;
	}
	if (nRate != m_nSampleRate)
	{
		Flush ();
		m_nSampleRate = nRate;
		m_nPhase = 0;
		m_nIn = m_nInAt = 0;
	}
	m_pStream->SetRate (nRate);
	return TRUE;
}

boolean CBTAudioOut::Open (void)
{
	return m_pStream->IsReady ();
}

unsigned CBTAudioOut::GetChunkFrames (void) const
{
	return m_nSampleRate == m_nOutRate ? m_nFrameSamples : InFrames;
}

unsigned CBTAudioOut::GetLatencyFrames (void) const
{
	return m_nSampleRate * LATENCY_MS / 1000;
}

// the stream's configuration: the encoder's
void CBTAudioOut::OnStream (void)
{
	if (!m_pStream->IsReady ())
	{
		m_bEncoder = FALSE;
		m_nPacket = 0;
		m_nPacketFrames = 0;
		return;
	}
	const CBTAVDTP::TConfig &C = m_pStream->GetConfig ();
	SBC_ENC_PARAMS *p = m_pEncoder;
	if (   m_bEncoder && m_nOutRate == C.nRate && p->s16NumOfBlocks == (SINT16) C.nBlocks
	    && p->s16NumOfSubBands == (SINT16) C.nSubbands && p->s16BitPool == (SINT16) C.nBitpool)
	{
		return;
	}
	memset (p, 0, sizeof *p);
	p->s16SamplingFreq = C.nRate == 48000 ? SBC_sf48000 : SBC_sf44100;
	p->s16ChannelMode = C.nChannels == 1 ? SBC_MONO : C.bJoint ? SBC_JOINT_STEREO : C.bDual ? SBC_DUAL : SBC_STEREO;
	p->s16NumOfChannels = (SINT16) C.nChannels;
	p->s16NumOfSubBands = (SINT16) C.nSubbands;
	p->s16NumOfBlocks = (SINT16) C.nBlocks;
	p->s16AllocationMethod = C.bSNR ? SBC_SNR : SBC_LOUDNESS;
	p->s16BitPool = (SINT16) C.nBitpool;
	p->u8NumPacketToEncode = 1;
	p->mSBCEnabled = 0;
	SBC_Encoder_Init (p);
	m_nOutRate = C.nRate;
	m_nFrameSamples = C.nBlocks * C.nSubbands;
	m_nPacket = 0;
	m_nPacketFrames = 0;
	m_bEncoder = TRUE;
}

// the next frames at the stream's rate: the ring's as they are, or between them
void CBTAudioOut::Fill (s16 *pFrames, unsigned nFrames)
{
	if (m_nSampleRate == m_nOutRate)
	{
		Take (pFrames, nFrames);
		return;
	}
	u32 nStep = (u32) (((u64) m_nSampleRate << 16) / m_nOutRate);
	for (unsigned i = 0; i < nFrames; i++)
	{
		while (m_nPhase >= 65536)
		{
			if (m_nInAt == m_nIn)
			{
				Take (m_In, InFrames);
				m_nIn = InFrames;
				m_nInAt = 0;
			}
			m_Previous[0] = m_Current[0];
			m_Previous[1] = m_Current[1];
			m_Current[0] = m_In[2 * m_nInAt];
			m_Current[1] = m_In[2 * m_nInAt + 1];
			m_nInAt++;
			m_nPhase -= 65536;
		}
		pFrames[2 * i] = (s16) (m_Previous[0] + (s32) (((s64) m_Current[0] - m_Previous[0]) * (s32) m_nPhase >> 16));
		pFrames[2 * i + 1] = (s16) (m_Previous[1] + (s32) (((s64) m_Current[1] - m_Previous[1]) * (s32) m_nPhase >> 16));
		m_nPhase += nStep;
	}
}

void CBTAudioOut::Frame (boolean bSend)
{
	s16 PCM[16 * 8 * 2];
	Fill (PCM, m_nFrameSamples);
	if (!bSend || !m_bEncoder)
	{
		return;
	}
	if (m_pEncoder->s16NumOfChannels == 1)		// (mono: the two channels' mean)
	{
		for (unsigned i = 0; i < m_nFrameSamples; i++)
		{
			PCM[i] = (s16) ((PCM[2 * i] + PCM[2 * i + 1]) / 2);
		}
	}

	// a media packet: the RTP header (12 bytes), how many SBC frames, the frames
	if (m_nPacket == 0)
	{
		u8 *p = m_Packet;
		p[0] = 0x80;
		p[1] = 0x60;
		p[2] = (u8) (m_nSequence >> 8);
		p[3] = (u8) m_nSequence;
		p[4] = (u8) (m_nTimestamp >> 24);
		p[5] = (u8) (m_nTimestamp >> 16);
		p[6] = (u8) (m_nTimestamp >> 8);
		p[7] = (u8) m_nTimestamp;
		p[8] = p[9] = p[10] = 0;
		p[11] = 1;
		m_nPacket = 13;
		m_nPacketFrames = 0;
	}
	m_pEncoder->ps16PcmBuffer = PCM;
	m_pEncoder->pu8Packet = m_Packet + m_nPacket;
	SBC_Encoder (m_pEncoder);
	unsigned nBytes = m_pEncoder->u16PacketLength;
	m_nPacket += nBytes;
	m_nPacketFrames++;
	m_nTimestamp += m_nFrameSamples;

	unsigned nMTU = m_pHost->GetMediaMTU ();
	unsigned nMost = nMTU < PACKET_BYTES ? nMTU : PACKET_BYTES;
	if (m_nPacketFrames == 15 || m_nPacket + nBytes > nMost)	// (no room for one more)
	{
		m_Packet[12] = (u8) m_nPacketFrames;
		if (m_pHost->SendMedia (m_Packet, m_nPacket))
		{
			m_nSent++;
		}
		else
		{
			m_nDropped++;
		}
		m_nSequence++;
		m_nPacket = 0;
	}
}

void CBTAudioOut::Update (void)
{
	unsigned nNow = CTimer::GetClockTicks ();
	unsigned nElapsed = nNow - m_nLast;
	m_nLast = nNow;

	// sound, or none for a while: the stream started, or suspended
	if (Queued ())
	{
		m_nLastSound = nNow;
	}
	if (HasSounds ())			// (sound effects: any moment)
	{
		m_nLastSound = nNow;
	}
	boolean bWanted = m_pStream->IsReady () && (Queued () || HasSounds () || (m_bWanted && nNow - m_nLastSound < IDLE_US));
	if (bWanted != m_bWanted)
	{
		m_bWanted = bWanted;
		m_nWanted = nNow;
		m_pStream->SetWanted (bWanted);
	}

	// The stream isn't running: the sound waits for it to start; or there's
	// none. But not for long (and not for a speaker that's gone): then the
	// sound is dropped at its pace, and what plays it goes on
	boolean bStreaming = m_pStream->IsStreaming () && m_bEncoder;
	if (!bStreaming && ((m_bWanted && nNow - m_nWanted < START_US) || !(Queued () || HasSounds ())))
	{
		m_nDue = 0;
		return;
	}
	if (nElapsed > m_nLongest)
	{
		m_nLongest = nElapsed;
	}
	if (nElapsed > MAX_BEHIND_US)
	{
		nElapsed = MAX_BEHIND_US;
		m_nSkips++;
	}
	m_nDue += (u64) nElapsed * m_nOutRate;
	u64 nFrame = (u64) m_nFrameSamples * 1000000;
	while (m_nDue >= nFrame)
	{
		Frame (bStreaming);
		m_nDue -= nFrame;
	}
}

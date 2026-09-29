//
// audio_out.h
//
// Sound on HDMI: 16-bit stereo frames played through the VideoCore's audio
// service (VCHIQ, Circle's vc4/sound), which sends them to the HDMI monitor
// when it takes audio. The frames come through a ring: one writer (the audio
// decoder, on another core where there is one), one reader (GetChunk, which
// VCHIQ's task calls for the next chunk); silence while the ring is empty.
//
#ifndef _gpu_audio_audio_out_h
#define _gpu_audio_audio_out_h

#include <vc4/sound/vchiqsoundbasedevice.h>
#include <vc4/vchiq/vchiqdevice.h>
#include <circle/types.h>

class CAudioOut : public CVCHIQSoundBaseDevice
{
public:
	static const unsigned RingFrames = 2 * 48000;		// 2 s at 48 kHz

	CAudioOut (CVCHIQDevice *pVCHIQ, unsigned nSampleRate);
	~CAudioOut (void);

	unsigned GetSampleRate (void) const	{ return m_nSampleRate; }

	/// \return Frames the ring has room for now
	unsigned Room (void) const;
	/// \brief Append frames (L, R) to the ring (the writer only)
	/// \return Frames taken (up to Room ())
	unsigned Write (const s16 *pFrames, unsigned nFrames);
	/// \return Frames waiting in the ring
	unsigned Queued (void) const;
	/// \brief Drop what waits in the ring (the reader's side stays consistent)
	void Flush (void);

	/// \return Frames written to the ring so far (mod 2^32)
	u32 GetWritten (void) const		{ return m_nIn; }
	/// \return Frames read from it so far (mod 2^32): the VideoCore has them
	u32 GetRead (void) const		{ return m_nOut; }

	/// \brief Paused: silence, the ring waits
	void SetPaused (boolean bPaused)	{ m_bPaused = bPaused; }

	/// \return Frames handed to the VideoCore so far (silence included)
	u64 GetFramesOut (void) const		{ return m_nFramesOut; }
	/// \return Frames of silence handed out because the ring was empty
	u64 GetUnderrunFrames (void) const	{ return m_nUnderrun; }

private:
	unsigned GetChunk (s16 *pBuffer, unsigned nChunkSize) override;

private:
	unsigned m_nSampleRate;
	s16 *m_pRing;				// RingFrames frames, 2 words each
	volatile unsigned m_nIn;		// frames written (mod 2^32), the writer's
	volatile unsigned m_nOut;		// frames read, the reader's
	volatile u64 m_nFramesOut;
	volatile u64 m_nUnderrun;
	volatile boolean m_bPaused;
};

#endif

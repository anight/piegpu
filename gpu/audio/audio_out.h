//
// audio_out.h
//
// Sound on HDMI: 16-bit stereo frames played through the VideoCore's audio
// service (VCHIQ, Circle's vc4/sound), which sends them to the HDMI monitor
// when it takes audio. The frames come through a ring: one writer (the audio
// decoder, on another core where there is one), one reader (GetChunk, which
// VCHIQ's task calls for the next chunk); silence while the ring is empty.
// The volume is applied as the chunks go to the VideoCore, not as the frames
// come into the ring: a change is heard after the two chunks the VideoCore
// holds (under 0.1 s), not after all that waits in the ring (seconds).
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
	/// \return When the last chunk was asked for (CTimer::GetClockTicks ())
	unsigned GetChunkTime (void) const	{ return m_nChunkTime; }
	/// \return Frames a chunk
	unsigned GetChunkFrames (void) const	{ return ChunkFrames; }
	static const unsigned ChunkFrames = 2048;

	/// \brief Paused: silence, the ring waits
	void SetPaused (boolean bPaused)	{ m_bPaused = bPaused; }
	/// \brief The volume, 0 (silent) to 65536 (the frames as they are): from
	///	    the next chunk, which goes to it smoothly from the last one's
	void SetVolume (unsigned nVolume)	{ m_nVolume = nVolume > 65536 ? 65536 : nVolume; }

	/// \return Frames handed to the VideoCore so far (silence included)
	u64 GetFramesOut (void) const		{ return m_nFramesOut; }
	/// \return Frames of silence handed out because the ring was empty
	u64 GetUnderrunFrames (void) const	{ return m_nUnderrun; }

	// the capture (debugging: the PCM host line, gpu/kernel.cpp): the last
	// CaptureSeconds of what GetChunk handed to the VideoCore, and each
	// chunk's time and how much of it came from the ring
	static const unsigned CaptureSeconds = 20;
	static const unsigned CaptureChunks = 4096;
	struct TChunk
	{
		u32 nTime;			// CTimer::GetClockTicks () when it was asked for
		u16 nFrames;			// handed out
		u16 nFromRing;			// of them from the ring (the rest: silence)
		u32 nQueued;			// frames in the ring before it
	};
	/// \brief Stop (or go on with) capturing, while it's being read
	void FreezeCapture (boolean bFrozen)	{ m_bCaptureFrozen = bFrozen; }
	/// \return The capture's frames in time order: the part at pFirst
	///	    (nFirst frames), then the part at pSecond (nSecond)
	void GetCapture (const s16 **pFirst, unsigned *nFirst, const s16 **pSecond, unsigned *nSecond) const;
	/// \return The chunks in time order, the same way
	void GetChunks (const TChunk **pFirst, unsigned *nFirst, const TChunk **pSecond, unsigned *nSecond) const;

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
	volatile unsigned m_nVolume;		// asked for (SetVolume)
	unsigned m_nGain;			// the last chunk's (GetChunk's)
	volatile unsigned m_nChunkTime;

	unsigned m_nCaptureFrames;
	s16 *m_pCapture;			// m_nCaptureFrames frames
	u32 m_nCaptured;			// frames captured so far
	TChunk *m_pChunks;			// CaptureChunks
	u32 m_nChunks;
	volatile boolean m_bCaptureFrozen;
};

#endif

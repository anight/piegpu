//
// audio_sink.h
//
// Where the sound goes: 16-bit stereo frames through a ring, one writer (the
// audio decoder, on another core where there is one), one reader (the
// output's: Take, a chunk at a time, at the pace the sound is played);
// silence while the ring is empty. The volume is applied as the frames are
// taken, not as they come into the ring: a change is heard after what the
// output holds, not after all that waits in the ring (seconds).
//
// The outputs: HDMI (audio_out.h), a Bluetooth speaker (bt/bt_audio.h).
//
#ifndef _gpu_audio_audio_sink_h
#define _gpu_audio_audio_sink_h

#include <circle/types.h>

class CSounds;

class CAudioSink
{
public:
	static const unsigned RingFrames = 2 * 48000;		// 2 s at 48 kHz

	CAudioSink (unsigned nSampleRate);
	virtual ~CAudioSink (void);

	unsigned GetSampleRate (void) const	{ return m_nSampleRate; }

	/// \brief Start playing what the ring gets
	/// \return FALSE if it can't
	virtual boolean Open (void) = 0;
	/// \brief Stop: silence from here
	virtual void Close (void) = 0;
	/// \return Frames taken from the ring at a time
	virtual unsigned GetChunkFrames (void) const = 0;
	/// \return Frames between one taken from the ring and its being heard
	virtual unsigned GetLatencyFrames (void) const = 0;

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
	/// \return Frames read from it so far (mod 2^32): the output has them
	u32 GetRead (void) const		{ return m_nOut; }
	/// \return When the last chunk was taken (CTimer::GetClockTicks ())
	unsigned GetChunkTime (void) const	{ return m_nChunkTime; }

	/// \brief Paused: silence, the ring waits
	void SetPaused (boolean bPaused)	{ m_bPaused = bPaused; }
	/// \brief The volume, 0 (silent) to 65536 (the frames as they are): from
	///	    the next chunk, which goes to it smoothly from the last one's
	void SetVolume (unsigned nVolume)	{ m_nVolume = nVolume > 65536 ? 65536 : nVolume; }

	/// \return Frames handed to the output so far (silence included)
	u64 GetFramesOut (void) const		{ return m_nFramesOut; }
	/// \return Frames of silence handed out because the ring was empty
	u64 GetUnderrunFrames (void) const	{ return m_nUnderrun; }

	/// \brief The sound effects (sounds.h), and the one output that plays
	///	   them: mixed into what it takes (the others play without)
	static void SetSounds (CSounds *pSounds, CAudioSink *pSink);
	/// \return TRUE if this output has effects to play (it's to keep running)
	boolean HasSounds (void) const;

protected:
	/// \brief The reader's: the next frames, silence where the ring has none
	/// \param pFromRing Frames of them that came from the ring
	/// \param pQueued Frames the ring held before
	void Take (s16 *pBuffer, unsigned nFrames, unsigned *pFromRing = nullptr, unsigned *pQueued = nullptr);

	unsigned m_nSampleRate;

private:
	s16 *m_pRing;				// RingFrames frames, 2 words each
	volatile unsigned m_nIn;		// frames written (mod 2^32), the writer's
	volatile unsigned m_nOut;		// frames read, the reader's
	volatile u64 m_nFramesOut;
	volatile u64 m_nUnderrun;
	volatile boolean m_bPaused;
	volatile unsigned m_nVolume;		// asked for (SetVolume)
	unsigned m_nGain;			// the last chunk's (Take's)
	volatile unsigned m_nChunkTime;

	static CSounds *s_pSounds;
	static CAudioSink *volatile s_pSoundsSink;
};

#endif

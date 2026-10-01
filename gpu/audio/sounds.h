//
// sounds.h
//
// Sound effects (docs/protocol.md 7.14): short sounds the host uploads once
// (mono, at their own rate) and plays on channels, each with its volume left
// and right; the channels are mixed into what the audio output plays (the
// stream's sound, or silence), as it's taken for playing: an effect is heard
// as soon as the output's own delay allows, not after what waits in the
// stream's ring. The host says which channel plays what: a sound on a channel
// replaces the one playing there.
//
// The commands come in the main loop; the mixing is done where the output
// takes its frames (CAudioSink::Take): HDMI's in the VideoCore service's
// task, the Bluetooth speaker's in the main loop. Both on core 0, and the
// tasks don't preempt: no lock.
//
#ifndef _gpu_audio_sounds_h
#define _gpu_audio_sounds_h

#include <pgpu_protocol.h>
#include <circle/types.h>

class CSounds
{
public:
	static const unsigned MaxSounds = PGPU_SOUNDS;
	static const unsigned Channels = PGPU_SOUND_CHANNELS;
	static const unsigned MaxFrames = 4 * 1024 * 1024;	// all the sounds' together (8 MB)

	CSounds (void);

	// the commands: 0 or a PGPU_ERR_* code
	/// \brief A part of a sound (the first part makes it, nFrames long)
	u32 Data (unsigned nId, unsigned nRate, unsigned nFrames, unsigned nOffset, u32 nFormat,
		  const u8 *pData, unsigned nBytes);
	/// \param nId 0: all of them
	u32 Delete (unsigned nId);
	/// \param nId 0: the channel stopped
	u32 Play (unsigned nChannel, unsigned nId, unsigned nLeft, unsigned nRight, u32 nFlags);
	u32 Volume (unsigned nChannel, unsigned nLeft, unsigned nRight);
	/// \brief Every channel stopped, every sound gone (a new session)
	void Reset (void);

	/// \return TRUE while the host has sounds here (an output is to run for them)
	boolean IsLoaded (void) const		{ return m_nLoaded != 0; }

	/// \brief The output's: the channels added to nFrames frames (L, R) at nRate
	/// \return FALSE if nothing was added (no channel plays)
	boolean Mix (s16 *pFrames, unsigned nFrames, unsigned nRate);

private:
	struct TSound
	{
		s16 *pFrames;			// nullptr: none
		unsigned nFrames;
		unsigned nRate;
	};
	struct TChannel
	{
		const TSound *pSound;		// nullptr: silent
		u64 nAt;			// where in it, frames << 16
		boolean bLoop;
		boolean bStopping;		// a loop told to stop: faded out first
		int nLeft, nRight;		// asked for, 0 .. 256
		int nNowLeft, nNowRight;	// as last mixed (a change is eased: no click)
	};

	TSound m_Sound[MaxSounds];
	TChannel m_Channel[Channels];
	unsigned m_nLoaded;
	unsigned m_nFrames;			// of all the sounds
};

#endif

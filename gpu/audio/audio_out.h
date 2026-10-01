//
// audio_out.h
//
// Sound on HDMI (an audio sink, audio_sink.h): the frames played through the
// VideoCore's audio service (VCHIQ, Circle's vc4/sound), which sends them to
// the HDMI monitor when it takes audio. VCHIQ's task asks for the next chunk
// (GetChunk); the VideoCore holds two of them (under 0.1 s).
//
#ifndef _gpu_audio_audio_out_h
#define _gpu_audio_audio_out_h

#include "audio_sink.h"
#include <vc4/sound/vchiqsoundbasedevice.h>
#include <vc4/vchiq/vchiqdevice.h>
#include <circle/types.h>

class CAudioOut : public CAudioSink
{
public:
	static const unsigned ChunkFrames = 2048;

	CAudioOut (CVCHIQDevice *pVCHIQ, unsigned nSampleRate);
	~CAudioOut (void);

	boolean Open (void) override			{ return m_Device.IsActive () || m_Device.Start (); }
	void Close (void) override			{ m_Device.Cancel (); }	// (waits for the VideoCore)
	unsigned GetChunkFrames (void) const override	{ return ChunkFrames; }
	unsigned GetLatencyFrames (void) const override	{ return 2 * ChunkFrames; }

	// the VideoCore's completions that came with flags (it ran dry: Circle's fork)
	unsigned GetCompleteFlagCount (void) const	{ return m_Device.GetCompleteFlagCount (); }
	unsigned GetLastCompleteFlags (void) const	{ return m_Device.GetLastCompleteFlags (); }
	unsigned GetLastCompleteFlagTime (void) const	{ return m_Device.GetLastCompleteFlagTime (); }

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
	class CDevice : public CVCHIQSoundBaseDevice		// the VideoCore's audio service
	{
	public:
		CDevice (CAudioOut *pOwner, CVCHIQDevice *pVCHIQ, unsigned nSampleRate);
	private:
		unsigned GetChunk (s16 *pBuffer, unsigned nChunkSize) override	{ return m_pOwner->GetChunk (pBuffer, nChunkSize); }
		CAudioOut *m_pOwner;
	};

	unsigned GetChunk (s16 *pBuffer, unsigned nChunkSize);

private:
	CDevice m_Device;

	unsigned m_nCaptureFrames;
	s16 *m_pCapture;			// m_nCaptureFrames frames
	u32 m_nCaptured;			// frames captured so far
	TChunk *m_pChunks;			// CaptureChunks
	u32 m_nChunks;
	volatile boolean m_bCaptureFrozen;
};

#endif

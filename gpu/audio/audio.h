//
// audio.h
//
// The audio stream (docs/protocol.md 7.13): AAC access units, MP3 frames or
// Ogg Vorbis pages from the host (MEDIA_DATA on stream 3) into a ring, decoded
// (aac.c, FAAD2; mp3.c, minimp3; vorbis.c, Tremor) to 16-bit stereo, played
// on HDMI through the VideoCore's audio service (CAudioOut), which applies the
// stream's volume. The sound is the clock
// of the video stream it plays with (CVideo::SetClock): GetTime says which
// time is being heard now.
//
// The decoder runs on a core of its own where there is one (DecodeLoop, from
// the kernel's CMultiCoreSupport), else in the main loop (Update). The rings
// between the cores have one writer and one reader each: the compressed
// samples (written by Data, core 0; a sample is the decoder's once its last
// chunk has come), the frames (CAudioOut) and the times (the decoder's).
//
#ifndef _gpu_audio_audio_h
#define _gpu_audio_audio_h

#include "audio_out.h"
#include "sounds.h"
#include "../video/video.h"
#include <vc4/vchiq/vchiqdevice.h>
#include <circle/types.h>

struct aac_s;
struct mp3_s;
struct vorbis_s;

class CAudioSinkOther : public CAudioSink		/// an output that's there only at times
{
public:
	CAudioSinkOther (unsigned nSampleRate) : CAudioSink (nSampleRate) {}
	/// \brief Sound of this rate is to come
	/// \return FALSE if it can't be played here now
	virtual boolean SetSource (unsigned nRate) = 0;
};

class CAudio : public CMediaClock
{
public:
	static const unsigned RingBytes = 512 * 1024;	// compressed data
	static const unsigned MaxSamples = 1024;	// (21 s of 48 kHz AAC, 26 s of 44.1 kHz MP3; Ogg pages: more)
	static const unsigned StatusWords = 12;

	CAudio (CVCHIQDevice *pVCHIQ, CVideo *pVideo);
	~CAudio (void);

	/// \param nPercent the volume a stream starts with (the volume= option)
	void SetDefaultVolume (unsigned nPercent);

	/// \brief The volume from now on: the default, and the stream's (core 0)
	void SetVolume (unsigned nPercent);
	unsigned GetDefaultVolume (void) const	{ return m_nDefaultVolume; }

	/// \brief Muted: silent whatever the volume, till it's taken off (the mute= setting)
	void SetMute (boolean bMute);
	boolean IsMuted (void) const		{ return m_bMute; }

	/// \brief A second of sound to hear the output by (Settings): a note on
	///	   the left, one on the right, one on both, at the volume as it is
	/// \return FALSE if a stream is playing, or there's no output
	boolean PlayTest (void);

	// the commands (docs/protocol.md 7.13): 0 or a PGPU_ERR_* code
	u32 Open (u32 nCodec, unsigned nVideoStream, const u8 *pConfig, unsigned nConfigBytes);
	u32 Data (u32 nFlags, s64 nPTS, unsigned nSampleBytes, const u8 *pData, unsigned nBytes);
	u32 Control (u32 nOp, s64 nArg);
	void Close (void);

	/// \brief Call often (core 0): decodes here if no core does
	void Update (void);

	/// \brief The decoder's core: never returns
	void DecodeLoop (void);

	/// \return The HDMI output the last stream played on (its capture), or nullptr
	CAudioOut *GetLastOut (void) const	{ return m_pLastOut; }

	/// \return The HDMI output for this rate (set up the first time), or
	///	    nullptr; it becomes the last output
	CAudioOut *GetHDMIOutput (unsigned nRate);

	/// \brief Another output (a Bluetooth speaker's): the sound goes there
	///	   while it takes it (its SetSource says so), else to HDMI
	void SetOther (class CAudioSinkOther *pOther)	{ m_pOther = pOther; }

	/// \return Where a stream of this rate goes now, or nullptr
	CAudioSink *GetOutput (unsigned nRate);

	// the sound effects' commands (docs/protocol.md 7.14: sounds.h): 0 or a PGPU_ERR_* code
	u32 SoundData (unsigned nId, unsigned nRate, unsigned nFrames, unsigned nOffset, u32 nFormat,
		       const u8 *pData, unsigned nBytes);
	u32 SoundDelete (unsigned nId);
	u32 SoundPlay (unsigned nChannel, unsigned nId, unsigned nLeft, unsigned nRight, u32 nFlags);
	u32 SoundVolume (unsigned nChannel, unsigned nLeft, unsigned nRight)	{ return m_Sounds.Volume (nChannel, nLeft, nRight); }
	/// \brief Every sound gone (a new session)
	void SoundReset (void);
	/// \brief Call often (core 0): the output for the effects, as there's a stream, a speaker, or neither
	void UpdateSounds (void);

	u32 SoundPitch (unsigned nChannel, u32 nPitch)				{ return m_Sounds.Pitch (nChannel, nPitch); }
	/// \brief The MEDIA_STATUS reply of the audio stream
	boolean GetStatus (u32 *pPayload, boolean bDue);

	/// \brief The time being heard now (the video's clock)
	/// \return FALSE before the sound has started
	boolean GetTime (s64 *pUS) override;

private:
	struct TSample
	{
		u32 nOffset;			// of its first byte (m_nBytesIn then)
		unsigned nSize;
		s64 nPTS;
		u32 nFlags;
	};
	struct TMark				// a decoded unit's time at its first frame
	{
		u32 nFrame;			// CAudioOut's frame count (Write) then
		s64 nPTS;
	};
	static const unsigned MaxMarks = 256;	// (5 s of 1024-frame units)
	static const unsigned MaxUnit = 65536;	// bytes of a unit (AAC: 6144 bits a channel; MP3: 2881; an Ogg page: 65307)
	static const unsigned MaxRates = 4;	// outputs kept, one a sample rate

	boolean DecodeOne (void);		// the decoder's step: FALSE if nothing to do
	boolean DecodePage (const TSample &S);	// Vorbis': FALSE while the page has more
	// the stream's codec's
	int Decode (const u8 *pUnit, unsigned nBytes, s16 *pFrames);
	void SetOutputVolume (void);
	const char *DecoderError (void);
	void CloseDecoder (void);
	static const char *CodecName (u32 nCodec);
	void CopyFromRing (u32 nOffset, u8 *pTo, unsigned nBytes) const;

private:
	CVCHIQDevice *m_pVCHIQ;
	CVideo *m_pVideo;
	unsigned m_nDefaultVolume;
	boolean m_bMute;
	boolean m_bOwnCore;			// the decoder runs on a core of its own

	// the stream (core 0 opens and closes it while the decoder is parked)
	volatile boolean m_bOpen;
	unsigned m_nVideoStream;
	u32 m_nCodec;				// PGPU_AUDIO_AAC, PGPU_AUDIO_MP3, PGPU_AUDIO_VORBIS
	struct aac_s *m_pAAC;			// the stream's decoder, one of these
	struct mp3_s *m_pMP3;
	struct vorbis_s *m_pVorbis;
	unsigned m_nMaxFrames;			// frames a unit decodes to, at most (Vorbis: a step)
	CAudioSink *m_pOut;			// the stream's
	CAudioOut *m_pOuts[MaxRates];		// HDMI's: set up once each (the VideoCore's service)
	CAudioOut *m_pLastOut;
	CAudioSink *m_pLastSink;		// the last used (the test sound's, with no stream)
	CAudioSinkOther *m_pOther;
	CSounds m_Sounds;			// the sound effects
	CAudioSink *m_pSoundSink;		// the output running for them, with no stream
	unsigned m_nRate;
	boolean m_bPaused;
	unsigned m_nVolume;			// percent
	u64 m_nSilenceAtOpen;			// the output's silent frames then
	u8 *m_pRing;

	// the samples: Data writes (core 0), the decoder reads
	u32 m_nBytesIn;				// written to the ring, since the open
	volatile u32 m_nSamplesIn;		// complete samples published
	TSample m_Samples[MaxSamples];
	boolean m_bPartial;			// m_Samples[m_nSamplesIn] is being written
	unsigned m_nPartialBytes;
	boolean m_bSkipping;			// a rejected sample's chunks are coming
	volatile u32 m_nBytesDone;		// the decoder's: freed
	volatile u32 m_nSamplesDone;

	// the times: the decoder writes, GetTime reads
	TMark m_Marks[MaxMarks];
	volatile u32 m_nMarks;

	// the decoder's state, and its handshake with core 0
	volatile boolean m_bPark;		// core 0 asks the decoder to stop touching the stream
	volatile boolean m_bParked;		// ... and it has
	volatile unsigned m_nDecoded, m_nErrors;
	volatile boolean m_bEOS;
	u8 *m_pUnit;				// one unit, contiguous
	s16 *m_pPCM;				// its frames
	boolean m_bPageFed;			// Vorbis: the sample being decoded is fed (a step at a time)
	unsigned m_nPageFrames;			// ... and has decoded to these frames so far
	unsigned m_nLosses;			// the decoder's packets lost, when last seen

	u64 m_nLastStatus;
	u64 m_nLastDebug;
};

#endif

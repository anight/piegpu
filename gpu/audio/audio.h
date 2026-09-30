//
// audio.h
//
// The audio stream (docs/protocol.md 7.13): AAC access units from the host
// (VIDEO_DATA on stream 3) into a ring, decoded (aac.c, FAAD2) to 16-bit
// stereo at the stream's volume, played on HDMI through the VideoCore's audio
// service (CAudioOut). The sound is the clock of the video stream it plays
// with (CVideo::SetClock): GetTime says which time is being heard now.
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
#include "../video/video.h"
#include <vc4/vchiq/vchiqdevice.h>
#include <circle/types.h>

struct aac_s;

class CAudio : public CMediaClock
{
public:
	static const unsigned RingBytes = 512 * 1024;	// compressed data
	static const unsigned MaxSamples = 1024;	// (21 s of 48 kHz AAC)
	static const unsigned StatusWords = 12;

	CAudio (CVCHIQDevice *pVCHIQ, CVideo *pVideo);
	~CAudio (void);

	/// \param nPercent the volume a stream starts with (the volume= option)
	void SetDefaultVolume (unsigned nPercent);

	// the commands (docs/protocol.md 7.13): 0 or a PGPU_ERR_* code
	u32 Open (u32 nCodec, unsigned nVideoStream, const u8 *pConfig, unsigned nConfigBytes);
	u32 Data (u32 nFlags, s64 nPTS, unsigned nSampleBytes, const u8 *pData, unsigned nBytes);
	u32 Control (u32 nOp, s64 nArg);
	void Close (void);

	/// \brief Call often (core 0): decodes here if no core does
	void Update (void);

	/// \brief The decoder's core: never returns
	void DecodeLoop (void);

	/// \return The output the last stream played on (its capture), or nullptr
	CAudioOut *GetLastOut (void) const	{ return m_pLastOut; }

	/// \return The output for this rate (set up the first time), or nullptr;
	///	    it becomes the last output
	CAudioOut *GetOutput (unsigned nRate);

	/// \brief The VIDEO_STATUS reply of the audio stream
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
	static const unsigned MaxUnit = 16384;	// bytes of an access unit (AAC: 6144 bits a channel)
	static const unsigned MaxRates = 4;	// outputs kept, one a sample rate

	boolean DecodeOne (void);		// the decoder's step: FALSE if nothing to do
	void CopyFromRing (u32 nOffset, u8 *pTo, unsigned nBytes) const;

private:
	CVCHIQDevice *m_pVCHIQ;
	CVideo *m_pVideo;
	unsigned m_nDefaultVolume;
	boolean m_bOwnCore;			// the decoder runs on a core of its own

	// the stream (core 0 opens and closes it while the decoder is parked)
	volatile boolean m_bOpen;
	unsigned m_nVideoStream;
	struct aac_s *m_pAAC;
	CAudioOut *m_pOut;			// the stream's (one of m_pOuts)
	CAudioOut *m_pOuts[MaxRates];		// set up once each (the VideoCore's service)
	CAudioOut *m_pLastOut;
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

	u64 m_nLastStatus;
	u64 m_nLastDebug;
};

#endif

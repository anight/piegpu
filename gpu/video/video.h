//
// video.h
//
// Video streams decoded by the VideoCore into textures (docs/protocol.md 7.12).
// A stream's compressed samples (H.264, with presentation times) come from the
// host in MEDIA_DATA packets into a ring; from there they go to the firmware's
// decoder (vc.ril.video_decode), which feeds its ISP (vc.ril.isp, scaling and
// YUV to RGBA) inside the VideoCore. The ISP's frames, at the texture's size,
// arrive in ARM memory (4 KB aligned: the V3D reads them as raster RGBA) in
// display order. At the end of each GL frame the texture is given the newest
// frame whose time has come (FrameEnd); the ones before it are dropped. So
// every draw of a frame sees the same video frame, whatever it's drawn on.
//
// MMAL's callbacks run in VCHIQ's tasks; they only queue buffers, which the
// main loop takes (Update).
//
#ifndef _video_h
#define _video_h

#include "../textures.h"
#include <circle/types.h>

struct MMAL_COMPONENT_T;
struct MMAL_CONNECTION_T;
struct MMAL_POOL_T;
struct MMAL_QUEUE_T;
struct MMAL_BUFFER_HEADER_T;

// a clock a video stream can follow (the sound it plays with)
class CMediaClock
{
public:
	virtual ~CMediaClock (void) {}
	/// \return FALSE while it isn't running yet
	virtual boolean GetTime (s64 *pUS) = 0;
};

class CVideo
{
public:
	static const unsigned MaxStreams = 2;			// ids 1 .. MaxStreams
	static const unsigned RingBytes = 4 * 1024 * 1024;	// compressed data, a stream
	static const unsigned StatusWords = 12;

public:
	CVideo (CTextures *pTextures);
	~CVideo (void);

	/// \brief Connect to the VideoCore's components (once, at boot)
	boolean Initialize (void);
	boolean IsInitialized (void) const	{ return m_bInitialized; }

	// the commands (docs/protocol.md 7.12): 0 or a PGPU_ERR_* code
	/// \param nFormat PGPU_VIDEO_ANNEXB, or PGPU_VIDEO_AVCC with pConfig the avcC
	u32 Open (unsigned nStream, u32 nCodec, u32 nTexture, unsigned nWidth, unsigned nHeight,
		  unsigned nCodedWidth, unsigned nCodedHeight, u32 nFormat, const u8 *pConfig,
		  unsigned nConfigBytes);
	/// \param nSampleBytes the whole sample's (with PGPU_MEDIA_FIRST): it's
	///	   taken whole or not at all
	u32 Data (unsigned nStream, u32 nFlags, s64 nPTS, unsigned nSampleBytes,
		  const u8 *pData, unsigned nBytes);
	u32 Control (unsigned nStream, u32 nOp, s64 nArg);
	void Close (unsigned nStream);
	/// \brief The texture gets another size; the decoding goes on (the frames
	///	   decoded but not shown yet are dropped)
	u32 Resize (unsigned nStream, unsigned nWidth, unsigned nHeight);
	void CloseAll (void);			// RESET

	/// \brief Call often: feeds the decoders, takes their frames
	void Update (void);

	/// \brief After a GL frame has rendered: the textures get the frames due now
	void FrameEnd (void);

	/// \brief A stream's frames follow this clock (nullptr: its own again).
	///	   Until the clock runs, the stream shows its first frame.
	void SetClock (unsigned nStream, CMediaClock *pClock);

	/// \brief The MEDIA_STATUS reply of a stream
	/// \param bDue only if its periodic one is due (else always)
	/// \return FALSE if there's none to send
	boolean GetStatus (unsigned nStream, u32 *pPayload, boolean bDue);

private:
	struct TSample				// in the ring, in decode order
	{
		unsigned nOffset;		// of its first byte in the ring
		unsigned nBytes;		// so far
		unsigned nSize;			// as its first chunk said
		unsigned nSent;			// to the decoder
		s64 nPTS;
		u32 nFlags;			// PGPU_MEDIA_* data flags
		boolean bComplete;		// its last chunk has come
	};
	static const unsigned MaxSamples = 256;

	struct TFrame
	{
		MMAL_BUFFER_HEADER_T *pBuffer;
		s64 nPTS;
	};
	static const unsigned MaxFrames = 8;	// decoded, waiting for their time

	struct TStream
	{
		boolean bOpen;
		u32 nTexture;
		unsigned nWidth, nHeight;	// the texture's

		MMAL_COMPONENT_T *pDecoder;
		MMAL_COMPONENT_T *pISP;
		MMAL_CONNECTION_T *pConnection;
		MMAL_POOL_T *pPoolIn;
		MMAL_POOL_T *pPoolOut;
		MMAL_QUEUE_T *pDecoded;		// from the ISP's callback

		u8 *pRing;
		unsigned nRingIn;		// where the next byte goes
		unsigned nRingUsed;
		u32 nBytesDone;			// sent to the decoder, since Open (mod 2^32)
		u32 nSamplesDone;
		boolean bSkipping;		// a rejected sample's chunks are coming
		TSample Samples[MaxSamples];
		unsigned nFirstSample, nSamples;

		TFrame Frames[MaxFrames];	// display order
		unsigned nFrames;
		TFrame Shown;			// the texture's (pBuffer nullptr: none)

		// the clock: media time = nMediaStart + (now - nClockStart) while playing
		boolean bPlaying;
		boolean bStarted;		// PLAY given, or the first frame started it
		s64 nMediaStart;
		u64 nClockStart;
		s64 nPausedAt;

		boolean bEOSSent, bEOS;		// sent to the decoder, came out of the ISP
		boolean bError;
		unsigned nDecoded, nShownFrames, nDropped;
		u64 nLastStatus;
		u64 nLastDebug;
	};

	static const s64 ResyncUS = 1000000;	// a gap in the times that moves the clock

	void Feed (TStream &S);
	void TakeFrames (TStream &S);
	s64 MediaTime (const TStream &S) const;
	void ShowDue (TStream &S, s64 nNow);
	void ReleaseFrame (TFrame *pFrame);
	void CopyFromRing (TStream &S, unsigned nOffset, u8 *pTo, unsigned nBytes);

	static void ControlCallback (struct MMAL_PORT_T *pPort, MMAL_BUFFER_HEADER_T *pBuffer);
	static void InputCallback (struct MMAL_PORT_T *pPort, MMAL_BUFFER_HEADER_T *pBuffer);
	static void OutputCallback (struct MMAL_PORT_T *pPort, MMAL_BUFFER_HEADER_T *pBuffer);

private:
	CTextures *m_pTextures;
	boolean m_bInitialized;
	TStream m_Streams[MaxStreams + 1];	// ids 1 .. MaxStreams
	CMediaClock *m_pClock[MaxStreams + 1];	// SetClock's (kept across Open and Close)
};

#endif

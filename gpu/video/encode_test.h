//
// encode_test.h
//
// Debugging (the ENC host line): the VideoCore's H.264 encoder, given the
// frames as they are shown. What it's for: how many frames a second it
// takes and gives, how many bytes, how late a frame comes out; and the
// stream, kept, to be looked at on a PC, and the sound played meanwhile (as
// the output took it: CAudioSink's tap), to go with it. Or both go into an
// MP4 file on the card as they come (mp4_writer.h): a recording.
//
#ifndef _video_encode_test_h
#define _video_encode_test_h

#include <circle/types.h>
#include "mp4_writer.h"

struct MMAL_PORT_T;
struct MMAL_BUFFER_HEADER_T;
struct MMAL_COMPONENT_T;
struct MMAL_POOL_T;

class CEncodeTest
{
public:
	CEncodeTest (void);
	~CEncodeTest (void);

	/// \brief Start encoding the frames given from now on (MMAL must be connected: CVideo::Initialize)
	/// \param nWidth, nHeight the frames' size (RGB565, rows top down)
	/// \param nFrameRate what the encoder is told to expect, a second
	/// \param nBitrate bits a second
	/// \param nSeconds for so long
	/// \param bI420 the frames are converted here to I420; else given as they are (RGB16)
	/// \param nKeepBytes so much of the stream's start is kept (GetStream), and the sound
	/// \param pFile an MP4 file for them, on the card (mounted); or nullptr
	boolean Start (unsigned nWidth, unsigned nHeight, unsigned nFrameRate, unsigned nBitrate,
		       unsigned nSeconds, boolean bI420, unsigned nKeepBytes, const char *pFile = nullptr);
	/// \brief Its time is over now (the next Update ends it)
	void End (void)				{ m_nEnd = 0; }
	boolean IsRunning (void) const		{ return m_bRunning; }
	unsigned GetWidth (void) const		{ return m_nWidth; }
	unsigned GetHeight (void) const		{ return m_nHeight; }

	/// \brief A frame was shown
	void Frame (const u16 *pRGB565);
	/// \brief A frame is about to be shown, and the output is still busy with
	///	   the one before: the time for a block to the card
	void FrameSlack (void);

	/// \brief The main loop's: a line a second
	/// \return TRUE once, when its time is over (the stream kept is there till the next Start)
	boolean Update (void);

	const u8 *GetStream (unsigned *pBytes) const	{ *pBytes = m_nKept; return m_pKeep; }
	/// \return Frames that came out, and in how long (what the stream's rate was)
	unsigned GetFrames (unsigned *pUs) const	{ *pUs = m_nTookUs; return m_Total.nFrames; }
	/// \return The sound meanwhile: 16-bit stereo frames (nullptr: none kept), how many, their rate
	const s16 *GetSound (unsigned *pFrames, unsigned *pRate) const	{ *pFrames = m_nSoundFrames; *pRate = m_nSoundRate; return m_pSound; }

private:
	void Stop (void);
	void Report (const char *pWhat, unsigned nUs);
	void TakeSound (void);

	static void ControlCallback (MMAL_PORT_T *pPort, MMAL_BUFFER_HEADER_T *pBuffer);
	static void InputCallback (MMAL_PORT_T *pPort, MMAL_BUFFER_HEADER_T *pBuffer);
	static void OutputCallback (MMAL_PORT_T *pPort, MMAL_BUFFER_HEADER_T *pBuffer);

private:
	boolean m_bRunning, m_bI420, m_bError;
	unsigned m_nWidth, m_nHeight, m_nStride, m_nRows;	// the frames'; the encoder's (rounded up)
	MMAL_COMPONENT_T *m_pEncoder;
	MMAL_POOL_T *m_pPoolIn, *m_pPoolOut;
	u64 m_nStart, m_nEnd, m_nLastReport;

	struct TCounts
	{
		unsigned nOffered, nSent, nNoBuffer;		// frames in
		unsigned nFrames, nKeyFrames, nBytes;		// frames out
		unsigned nBiggest, nBiggestKey;			// a frame's bytes
		unsigned nGivingUs;				// converting, copying, sending
		u64 nLateUs;					// in to out, summed
		unsigned nLatestUs;				// the most
	};
	TCounts m_Second, m_Total;
	unsigned m_nFrameBytes;					// of the frame coming out

	u8 *m_pKeep;
	unsigned m_nKeepBytes, m_nKept;
	unsigned m_nTookUs;
	s16 *m_pSound;					// kept (with the stream)
	unsigned m_nSoundMax, m_nSoundFrames, m_nSoundRate;
	boolean m_bTapped, m_bSoundStarted;
	u64 m_nFirstFrameUs;				// when the first frame was given

	CMp4Writer m_File;
	u8 *m_pFrame;					// the frame coming out, whole (for the file)
	unsigned m_nFrameMax, m_nFrameFill;
	u64 m_nLastFileWork;
};

#endif

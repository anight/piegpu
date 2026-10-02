//
// mp4_writer.h
//
// An MP4 file written to the card as it is recorded: H.264 frames as they
// come from the encoder and the sound as 16-bit PCM ("sowt": there is no AAC
// encoder here), in chunks of about half a second, the video's first; the
// index (moov) at the end, when the recording is closed. What comes goes
// into a ring; Work (), from the main loop, writes a block of it at a time.
// A recording not closed (the power gone) has no index: it can't be played.
//
#ifndef _gpu_video_mp4_writer_h
#define _gpu_video_mp4_writer_h

#include <circle/types.h>
#include <fatfs/ff.h>

class CMp4Writer
{
public:
	static const unsigned RingBytes = 8 * 1024 * 1024;	// waiting for the card
	// written at a time, whole sectors: the main loop waits for it. Measured (a
	// Zero 2 W, the card's SDHOST driver): 4 ms for 16 KB, 8 ms for 64 KB. The
	// small ones fit into a frame's wait for the panel (CEncodeTest::FrameSlack)
	static const unsigned BlockBytes = 16 * 1024;
	static const unsigned ChunkFrames = 24000;		// the sound of a chunk (0.5 s at 48 kHz)

	CMp4Writer (void);
	~CMp4Writer (void);

	/// \param pPath the file (the card is mounted)
	boolean Open (const char *pPath, unsigned nWidth, unsigned nHeight);
	boolean IsOpen (void) const		{ return m_bOpen; }
	/// \return FALSE once a write failed or the ring overflowed (the card too slow): Close it
	boolean IsGood (void) const		{ return !m_bFailed; }

	/// \brief The stream's parameter sets (Annex B: SPS, PPS); the first of each are the file's
	void Config (const u8 *pData, unsigned nBytes);
	/// \brief A frame (Annex B, whole), shown at nTimeUs (any origin)
	void Video (const u8 *pData, unsigned nBytes, boolean bKey, u64 nTimeUs);
	/// \brief Sound that goes on from the last (L, R frames); one rate for the file
	void Sound (const s16 *pFrames, unsigned nFrames, unsigned nRate);

	/// \brief The main loop's: a block to the card, if one waits
	void Work (void);
	/// \brief The rest and the index to the card
	/// \return FALSE if the file is no good
	boolean Close (void);

	unsigned GetVideoFrames (void) const	{ return m_nVideo; }
	u64 GetBytes (void) const		{ return m_nWritten; }
	/// \brief The card's writes so far: how many, their time together and the longest (us), the most waiting (bytes)
	void GetWrites (unsigned *pCount, u64 *pUs, unsigned *pLongestUs, unsigned *pMostWaiting) const;

private:
	struct TChunk
	{
		u32 nOffset;			// in the file
		u32 nSamples;			// video: frames; sound: frames
	};

	void Put (const void *pData, unsigned nBytes);
	void Put32 (u32 nValue);
	void CutSound (void);			// the sound waiting becomes a chunk
	boolean WriteBlock (unsigned nBytes);
	template <class T> static boolean Grow (T **ppArray, unsigned *pMax, unsigned nNeeded);
	void BuildIndex (class CBoxes &rBoxes);

private:
	boolean m_bOpen, m_bFailed;
	FIL m_File;
	unsigned m_nWidth, m_nHeight;

	u8 *m_pRing;
	u64 m_nIn, m_nOut;			// bytes put, bytes written (of the data after the header)
	u64 m_nWritten;
	u32 m_nOffset;				// the file's, of what's put next

	u8 m_SPS[128], m_PPS[64];
	unsigned m_nSPS, m_nPPS;

	// the video's frames: two words each, the size (bit 31: a key frame) and the
	// time since the first one's, microseconds (in the end: its duration)
	u32 *m_pSizes;
	unsigned m_nVideo, m_nVideoMax;		// frames; words there's room for
	u64 m_nFirstTime;
	TChunk *m_pVideoChunks;
	unsigned m_nVideoChunks, m_nVideoChunksMax;
	boolean m_bInVideoChunk;

	// the sound: what waits for its chunk
	s16 *m_pSound;
	unsigned m_nSoundWaiting, m_nSoundFrames, m_nRate;
	TChunk *m_pSoundChunks;
	unsigned m_nSoundChunks, m_nSoundChunksMax;

	unsigned m_nWrites, m_nLongestUs, m_nMostWaiting;
	u64 m_nWriteUs;
};

#endif

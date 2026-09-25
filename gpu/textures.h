//
// textures.h
//
// Texture objects (docs/protocol.md 7.4). All formats are converted at upload
// to the TMU's raster RGBA8888 format (RGBA32R, R in byte 0), so no tiling
// is needed; ETC1 is decoded on the ARM. Mipmaps are not supported (v1).
//
// Copy-on-write: uploading to a texture that a draw of the current frame
// uses gives it new storage; the old one is freed after the frame.
//
#ifndef _textures_h
#define _textures_h

#include <circle/types.h>

class CTextures
{
public:
	static const unsigned MaxTextures = 128;
	static const unsigned MaxSize = 1024;
	static const unsigned MaxTotalBytes = 32 * 1024 * 1024;

public:
	CTextures (void);
	~CTextures (void);

	void Reset (void);

	// commands; return 0 or a pgpu_error code
	u32 Create (u32 nId, unsigned nWidth, unsigned nHeight, u32 nFormat);
	u32 Data (u32 nId, unsigned x, unsigned y, unsigned nWidth, unsigned nHeight,
		  const u32 *pData, unsigned nWords);
	u32 Params (u32 nId, u32 nMinFilter, u32 nMagFilter, u32 nWrapS, u32 nWrapT);
	u32 Delete (u32 nId);

	/// \brief Get the TMU config uniforms and mark the texture used by this frame
	/// \return FALSE if there is no such texture
	boolean Use (u32 nId, u32 *pP0, u32 *pP1);

	/// \brief TMU config of a 1x1 texture reading (0, 0, 0, 1), for samplers
	/// without a texture
	void UseFallback (u32 *pP0, u32 *pP1);

	/// \brief Free storage replaced during the frame
	void EndFrame (void);

private:
	struct TStorage
	{
		u8 *pRaw;		// as allocated
		u32 *pPixels;		// 4 KB aligned, RGBA8888
	};

	struct TTexture
	{
		boolean bValid;
		unsigned nWidth, nHeight;
		unsigned nStride;	// pixels per row in storage (width, at least 4)
		u32 nFormat;
		TStorage Storage;
		boolean bUsed;		// by a draw of the current frame
		u32 nP1Filters;		// filter and wrap bits of config P1
	};

	boolean Alloc (TTexture *pTexture, TStorage *pStorage);
	void Free (TStorage *pStorage);
	void Retire (TStorage *pStorage);

	static void DecodeETC1 (const u8 *pBlock, u32 Out[16]);

private:
	TTexture m_Textures[MaxTextures + 1];	// ids 1 .. MaxTextures
	unsigned m_nTotalBytes;

	static const unsigned MaxRetired = 64;
	TStorage m_Retired[MaxRetired];
	unsigned m_nRetired;
};

#endif

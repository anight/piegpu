//
// textures.h
//
// Texture objects (docs/protocol.md 7.4). All formats are converted at upload
// to RGBA8888 (R in byte 0) in the V3D's tiled layouts, as Mesa's vc4 does
// (vc4_resource.c, vc4_tiling.c): each mip level is LT (small levels) or
// T-format (4 KB tiles), the levels are stored smallest first with level 0
// page aligned, and the faces of a cube map are whole mip trees at a page
// aligned stride. ETC1 is decoded on the ARM.
//
// Power-of-two textures have a full mip chain; non-power-of-two textures
// have only level 0 (as in GL ES 2.0). A texture that isn't complete for
// its filters (GL rules) can't be used: Use () returns FALSE.
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
	static const unsigned MaxSize = 2048;
	static const unsigned MaxLevels = 12;		// 2048 .. 1
	static const unsigned MaxTotalBytes = 32 * 1024 * 1024;

	// texture config words and properties, from Use ()
	struct TConfig
	{
		u32 P0, P1, P2;
		boolean bCube;
		boolean bAlphaFormat;		// ALPHA: RGB is 0 (fixed-function: see shaders.py)
	};

public:
	CTextures (void);
	~CTextures (void);

	void Reset (void);

	// commands; return 0 or a pgpu_error code
	u32 Create (u32 nId, unsigned nWidth, unsigned nHeight, u32 nFormat, boolean bCube);
	u32 Data (u32 nId, unsigned nLevel, unsigned nFace, unsigned x, unsigned y,
		  unsigned nWidth, unsigned nHeight, const u32 *pData, unsigned nWords);
	u32 Params (u32 nId, u32 nMinFilter, u32 nMagFilter, u32 nWrapS, u32 nWrapT);
	u32 GenerateMipmap (u32 nId);
	u32 Delete (u32 nId);

	/// \brief Get the TMU config and mark the texture used by this frame
	/// \return FALSE if there is no such texture or it isn't complete
	boolean Use (u32 nId, TConfig *pConfig);

	/// \brief TMU config of a 1x1 texture reading (0, 0, 0, 1), for samplers
	/// without a (complete) texture
	void UseFallback (TConfig *pConfig);

	/// \brief Level 0 (of a cube face) as a render target: bus address, size,
	/// T-format (else LT); marks the level defined
	/// \return FALSE if there is no such texture or face
	boolean GetRenderTarget (u32 nId, unsigned nFace, u32 *pBus, unsigned *pWidth,
				 unsigned *pHeight, boolean *pTFormat);

	/// \brief Read a texel (RGBA8888) after the V3D has rendered into the texture
	/// (call Invalidate () first)
	u32 ReadRGBA (u32 nId, unsigned nFace, unsigned x, unsigned y);
	void Invalidate (u32 nId);

	/// \brief Write RGBA8888 texels to a level, taken as the texture's base format
	/// (glCopyTexImage2D; copy-on-write as TEXTURE_DATA)
	u32 WriteRGBA (u32 nId, unsigned nLevel, unsigned nFace, unsigned x, unsigned y,
		       unsigned nWidth, unsigned nHeight, const u32 *pPixels);

	/// \brief Free storage replaced during the job (after it has rendered)
	void EndFrame (void);

private:
	struct TStorage
	{
		u8 *pRaw;		// as allocated
		u8 *pBase;		// 4 KB aligned
	};

	struct TLevel
	{
		unsigned nWidth, nHeight;	// as sampled
		unsigned nOffset;		// from the base of a face
		unsigned nStride;		// bytes per pixel row
		boolean bT;			// T-format, else LT
	};

	struct TTexture
	{
		boolean bValid;
		unsigned nWidth, nHeight;
		u32 nFormat;
		boolean bCube;
		unsigned nLevels;		// in storage
		TLevel Levels[MaxLevels];
		unsigned nLevel0;		// offset of level 0 (page aligned)
		unsigned nFaceStride;		// bytes per face (cube map stride)
		unsigned nBytes;		// all faces
		u32 nDefined[6];		// levels with data, per face
		TStorage Storage;
		boolean bUsed;			// by a draw of the current frame
		u32 nMinFilter, nMagFilter, nWrapS, nWrapT;
	};

	void Layout (TTexture *pTexture);
	u32 *TexelAddress (TTexture *pTexture, unsigned nFace, unsigned nLevel, unsigned x, unsigned y);
	boolean Complete (const TTexture *pTexture) const;
	boolean Alloc (TTexture *pTexture, TStorage *pStorage);
	void Free (TStorage *pStorage);
	void Retire (TStorage *pStorage);
	boolean CopyOnWrite (TTexture *pTexture);

	static void DecodeETC1 (const u8 *pBlock, u32 Out[16]);

private:
	TTexture m_Textures[MaxTextures + 1];	// ids 1 .. MaxTextures
	unsigned m_nTotalBytes;

	static const unsigned MaxRetired = 64;
	TStorage m_Retired[MaxRetired];
	unsigned m_nRetired;

	u32 *m_pFallback;
};

#endif

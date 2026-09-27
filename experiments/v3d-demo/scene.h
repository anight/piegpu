//
// scene.h
//
// The demo scene (plasma background + lit rotating cube, same as gles-demo)
// rendered by the V3D in NV ("no vertex shading") mode: the ARM transforms
// the 36 cube vertices, the QPUs run the fragment shaders.
//
#ifndef _scene_h
#define _scene_h

#include "v3d.h"
#include <circle/types.h>

class CScene
{
public:
	CScene (CV3D *pV3D);
	~CScene (void);

	/// \brief Allocate buffers for up to this resolution
	boolean Initialize (unsigned nMaxWidth, unsigned nMaxHeight);

	static const unsigned FrameBuffers = 2;

	/// \brief Build the control lists for a resolution
	/// \param bRGB565 Store RGB565 (little endian, dithered) instead of BGRA8888
	boolean SetResolution (unsigned nWidth, unsigned nHeight, boolean bRGB565 = FALSE);

	/// \brief Render one frame into frame buffer nBuffer and wait for completion
	boolean Render (float fTime, unsigned nBuffer = 0,
			unsigned *pBinUs = nullptr, unsigned *pRenderUs = nullptr);

	/// \return Frame buffer (V3D accessible; no cache maintenance done)
	void *GetFrameBuffer (unsigned nBuffer) const	{ return m_pFrameBuffer[nBuffer]; }

private:
	void UpdatePlasmaUniforms (float fTime);
	void UpdateCubeVertices (float fTime);

private:
	CV3D *m_pV3D;

	unsigned m_nWidth;
	unsigned m_nHeight;

	u8 *m_pBinCL;
	u8 *m_pRenderCL[FrameBuffers];
	u32 m_nBinStart, m_nBinEnd;
	u32 m_nRenderStart[FrameBuffers], m_nRenderEnd[FrameBuffers];

	u8 *m_pShaderRecords;
	u32 *m_pCubeCode;
	u32 *m_pPlasmaCode;
	float *m_pUniforms;
	u8 *m_pPlasmaVertices;
	u8 *m_pCubeVertices;

	u8 *m_pTileAlloc;
	u8 *m_pTileState;
	u8 *m_pOverflow;
	void *m_pFrameBuffer[FrameBuffers];

	size_t m_nFrameBufferSize;
};

#endif

//
// renderer.h
//
// OpenGL ES 2.0 demo scene (plasma background shader + lit rotating cube),
// rendered by the VideoCore IV into an offscreen EGL pbuffer.
//
#ifndef _renderer_h
#define _renderer_h

#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <circle/types.h>

class CRenderer
{
public:
	CRenderer (void);
	~CRenderer (void);

	/// \brief Initialize EGL and create the GL context
	boolean Initialize (void);

	/// \brief Create an offscreen render target and make it current
	boolean SetSurface (unsigned nWidth, unsigned nHeight);

	/// \brief Render one frame of the scene
	/// \param fTime Animation time in seconds
	void Render (float fTime);

	/// \brief Wait until the GPU has finished rendering
	void Finish (void);

	/// \brief Read the frame back and convert it to big endian RGB565 (top row first)
	/// \param pBuffer Destination buffer (width * height u16)
	/// \return Microseconds spent in glReadPixels
	unsigned ReadPixelsRGB565BE (u16 *pBuffer);

	const char *GetRenderer (void) const	{ return m_pRenderer; }

private:
	static GLuint CompileShader (GLenum Type, const char *pSource);
	static GLuint LinkProgram (const char *pVertex, const char *pFragment);

private:
	EGLDisplay m_Display;
	EGLConfig m_Config;
	EGLContext m_Context;
	EGLSurface m_Surface;

	unsigned m_nWidth;
	unsigned m_nHeight;

	GLuint m_PlasmaProgram;
	GLint m_PlasmaTime;
	GLint m_PlasmaAspect;

	GLuint m_CubeProgram;
	GLint m_CubeModel;
	GLint m_CubeProj;

	GLuint m_QuadBuffer;
	GLuint m_CubeBuffer;

	u8 *m_pReadBuffer;

	const char *m_pRenderer;
};

#endif

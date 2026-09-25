//
// renderer.cpp
//
#include "renderer.h"
#include <circle/logger.h>
#include <circle/timer.h>
#include <circle/util.h>
#include <math.h>
#include <assert.h>

extern "C"
{
#include <bcm_host.h>
}

LOGMODULE ("renderer");

static const char PlasmaVertexShader[] =
	"attribute vec2 a_pos;\n"
	"varying vec2 v_pos;\n"
	"uniform float u_aspect;\n"
	"void main ()\n"
	"{\n"
	"	v_pos = vec2 (a_pos.x * u_aspect, a_pos.y);\n"
	"	gl_Position = vec4 (a_pos, 0.0, 1.0);\n"
	"}\n";

static const char PlasmaFragmentShader[] =
	"precision mediump float;\n"
	"varying vec2 v_pos;\n"
	"uniform float u_time;\n"
	"void main ()\n"
	"{\n"
	"	vec2 p = v_pos * 3.0;\n"
	"	float v = sin (p.x + u_time)\n"
	"		+ sin (p.y * 1.3 + u_time * 0.7)\n"
	"		+ sin ((p.x + p.y) * 0.8 + u_time * 1.3)\n"
	"		+ sin (length (p) * 1.5 - u_time);\n"
	"	vec3 c = 0.5 + 0.5 * cos (vec3 (0.0, 2.1, 4.2) + v * 1.2);\n"
	"	gl_FragColor = vec4 (c * 0.55, 1.0);\n"
	"}\n";

static const char CubeVertexShader[] =
	"attribute vec3 a_pos;\n"
	"attribute vec3 a_normal;\n"
	"attribute vec3 a_color;\n"
	"uniform mat4 u_model;\n"
	"uniform mat4 u_proj;\n"
	"varying vec3 v_color;\n"
	"void main ()\n"
	"{\n"
	"	vec3 n = normalize ((u_model * vec4 (a_normal, 0.0)).xyz);\n"
	"	float diffuse = max (dot (n, normalize (vec3 (0.4, 0.7, 1.0))), 0.0);\n"
	"	v_color = a_color * (0.25 + 0.75 * diffuse);\n"
	"	gl_Position = u_proj * u_model * vec4 (a_pos, 1.0);\n"
	"}\n";

static const char CubeFragmentShader[] =
	"precision mediump float;\n"
	"varying vec3 v_color;\n"
	"void main ()\n"
	"{\n"
	"	gl_FragColor = vec4 (v_color, 1.0);\n"
	"}\n";

// Full-screen quad as triangle strip
static const GLfloat QuadVertices[] = {-1, -1,  1, -1,  -1, 1,  1, 1};

// Cube: 6 faces * 2 triangles * 3 vertices, each: position, normal, color
static GLfloat CubeVertices[36 * 9];

static void BuildCube (void)
{
	static const struct
	{
		GLfloat n[3];		// face normal
		GLfloat u[3], v[3];	// face axes
		GLfloat c[3];		// face color
	}
	Faces[6] =
	{
		{{ 1, 0, 0}, {0, 1, 0}, {0, 0, 1}, {1.0f, 0.3f, 0.3f}},
		{{-1, 0, 0}, {0, 0, 1}, {0, 1, 0}, {0.3f, 1.0f, 0.3f}},
		{{ 0, 1, 0}, {0, 0, 1}, {1, 0, 0}, {0.3f, 0.4f, 1.0f}},
		{{ 0,-1, 0}, {1, 0, 0}, {0, 0, 1}, {1.0f, 1.0f, 0.3f}},
		{{ 0, 0, 1}, {1, 0, 0}, {0, 1, 0}, {0.3f, 1.0f, 1.0f}},
		{{ 0, 0,-1}, {0, 1, 0}, {1, 0, 0}, {1.0f, 0.3f, 1.0f}},
	};
	static const GLfloat Corners[6][2] = {{-1,-1}, {1,-1}, {1,1},  {-1,-1}, {1,1}, {-1,1}};

	GLfloat *p = CubeVertices;
	for (unsigned f = 0; f < 6; f++)
	{
		for (unsigned i = 0; i < 6; i++)
		{
			for (unsigned k = 0; k < 3; k++)
			{
				*p++ = 0.5f * (Faces[f].n[k] + Corners[i][0] * Faces[f].u[k]
							     + Corners[i][1] * Faces[f].v[k]);
			}
			for (unsigned k = 0; k < 3; k++) *p++ = Faces[f].n[k];
			for (unsigned k = 0; k < 3; k++) *p++ = Faces[f].c[k];
		}
	}
}

// Column-major 4x4 matrices
static void MatMultiply (GLfloat *pResult, const GLfloat *a, const GLfloat *b)
{
	GLfloat r[16];
	for (unsigned c = 0; c < 4; c++)
	{
		for (unsigned row = 0; row < 4; row++)
		{
			r[c*4 + row] =   a[0*4 + row] * b[c*4 + 0] + a[1*4 + row] * b[c*4 + 1]
				       + a[2*4 + row] * b[c*4 + 2] + a[3*4 + row] * b[c*4 + 3];
		}
	}
	memcpy (pResult, r, sizeof r);
}

static void MatRotation (GLfloat *m, float fAngleX, float fAngleY, float fDistance)
{
	float sx = sinf (fAngleX), cx = cosf (fAngleX);
	float sy = sinf (fAngleY), cy = cosf (fAngleY);

	GLfloat Rx[16] = {1, 0, 0, 0,  0, cx, sx, 0,  0, -sx, cx, 0,  0, 0, 0, 1};
	GLfloat Ry[16] = {cy, 0, -sy, 0,  0, 1, 0, 0,  sy, 0, cy, 0,  0, 0, 0, 1};

	MatMultiply (m, Rx, Ry);
	m[14] = -fDistance;		// translate along -z
}

static void MatPerspective (GLfloat *m, float fFovY, float fAspect, float fNear, float fFar)
{
	float f = 1.0f / tanf (fFovY / 2.0f);

	memset (m, 0, 16 * sizeof (GLfloat));
	m[0] = f / fAspect;
	m[5] = f;
	m[10] = (fFar + fNear) / (fNear - fFar);
	m[11] = -1.0f;
	m[14] = 2.0f * fFar * fNear / (fNear - fFar);
}

CRenderer::CRenderer (void)
:	m_Display (EGL_NO_DISPLAY),
	m_Context (EGL_NO_CONTEXT),
	m_Surface (EGL_NO_SURFACE),
	m_nWidth (0),
	m_nHeight (0),
	m_pReadBuffer (nullptr),
	m_pRenderer ("?")
{
}

CRenderer::~CRenderer (void)
{
	delete [] m_pReadBuffer;
}

boolean CRenderer::Initialize (void)
{
	bcm_host_init ();

	m_Display = eglGetDisplay (EGL_DEFAULT_DISPLAY);
	if (   m_Display == EGL_NO_DISPLAY
	    || !eglInitialize (m_Display, nullptr, nullptr))
	{
		LOGERR ("Cannot initialize EGL");
		return FALSE;
	}

	static const EGLint ConfigAttribs[] =
	{
		EGL_RED_SIZE, 8,
		EGL_GREEN_SIZE, 8,
		EGL_BLUE_SIZE, 8,
		EGL_ALPHA_SIZE, 8,
		EGL_DEPTH_SIZE, 16,
		EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
		EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
		EGL_NONE
	};

	EGLint nConfigs;
	if (   !eglChooseConfig (m_Display, ConfigAttribs, &m_Config, 1, &nConfigs)
	    || nConfigs < 1)
	{
		LOGERR ("No suitable EGL config");
		return FALSE;
	}

	eglBindAPI (EGL_OPENGL_ES_API);

	static const EGLint ContextAttribs[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
	m_Context = eglCreateContext (m_Display, m_Config, EGL_NO_CONTEXT, ContextAttribs);
	if (m_Context == EGL_NO_CONTEXT)
	{
		LOGERR ("Cannot create GL context");
		return FALSE;
	}

	return TRUE;
}

boolean CRenderer::SetSurface (unsigned nWidth, unsigned nHeight)
{
	boolean bFirst = m_Surface == EGL_NO_SURFACE;

	if (!bFirst)
	{
		eglMakeCurrent (m_Display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
		eglDestroySurface (m_Display, m_Surface);
	}

	const EGLint SurfaceAttribs[] = {EGL_WIDTH, (EGLint) nWidth, EGL_HEIGHT, (EGLint) nHeight, EGL_NONE};
	m_Surface = eglCreatePbufferSurface (m_Display, m_Config, SurfaceAttribs);
	if (m_Surface == EGL_NO_SURFACE)
	{
		LOGERR ("Cannot create %ux%u pbuffer (0x%X)", nWidth, nHeight, eglGetError ());
		return FALSE;
	}

	if (!eglMakeCurrent (m_Display, m_Surface, m_Surface, m_Context))
	{
		LOGERR ("Cannot make context current");
		return FALSE;
	}

	m_nWidth = nWidth;
	m_nHeight = nHeight;

	delete [] m_pReadBuffer;
	m_pReadBuffer = new u8[nWidth * nHeight * 4];

	glViewport (0, 0, nWidth, nHeight);

	if (!bFirst)
	{
		return TRUE;
	}

	// One-time GL setup (objects are kept by the context)

	m_pRenderer = (const char *) glGetString (GL_RENDERER);
	LOGNOTE ("GL_VENDOR: %s, GL_RENDERER: %s", glGetString (GL_VENDOR), m_pRenderer);
	LOGNOTE ("GL_VERSION: %s, GLSL: %s", glGetString (GL_VERSION),
		 glGetString (GL_SHADING_LANGUAGE_VERSION));

	m_PlasmaProgram = LinkProgram (PlasmaVertexShader, PlasmaFragmentShader);
	m_CubeProgram = LinkProgram (CubeVertexShader, CubeFragmentShader);
	if (!m_PlasmaProgram || !m_CubeProgram)
	{
		return FALSE;
	}

	m_PlasmaTime = glGetUniformLocation (m_PlasmaProgram, "u_time");
	m_PlasmaAspect = glGetUniformLocation (m_PlasmaProgram, "u_aspect");
	m_CubeModel = glGetUniformLocation (m_CubeProgram, "u_model");
	m_CubeProj = glGetUniformLocation (m_CubeProgram, "u_proj");

	BuildCube ();

	glGenBuffers (1, &m_QuadBuffer);
	glBindBuffer (GL_ARRAY_BUFFER, m_QuadBuffer);
	glBufferData (GL_ARRAY_BUFFER, sizeof QuadVertices, QuadVertices, GL_STATIC_DRAW);

	glGenBuffers (1, &m_CubeBuffer);
	glBindBuffer (GL_ARRAY_BUFFER, m_CubeBuffer);
	glBufferData (GL_ARRAY_BUFFER, sizeof CubeVertices, CubeVertices, GL_STATIC_DRAW);

	glEnable (GL_CULL_FACE);

	return glGetError () == GL_NO_ERROR;
}

void CRenderer::Render (float fTime)
{
	float fAspect = (float) m_nWidth / m_nHeight;

	glClear (GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

	// Background: plasma shader on a full-screen quad
	glDisable (GL_DEPTH_TEST);
	glUseProgram (m_PlasmaProgram);
	glUniform1f (m_PlasmaTime, fTime);
	glUniform1f (m_PlasmaAspect, fAspect);
	glBindBuffer (GL_ARRAY_BUFFER, m_QuadBuffer);
	glVertexAttribPointer (0, 2, GL_FLOAT, GL_FALSE, 0, 0);
	glEnableVertexAttribArray (0);
	glDrawArrays (GL_TRIANGLE_STRIP, 0, 4);
	glDisableVertexAttribArray (0);

	// Foreground: lit rotating cube
	glEnable (GL_DEPTH_TEST);
	glUseProgram (m_CubeProgram);

	GLfloat Model[16], Proj[16];
	MatRotation (Model, fTime * 0.9f, fTime * 1.3f, 3.0f);
	MatPerspective (Proj, 0.9f, fAspect, 0.5f, 10.0f);
	glUniformMatrix4fv (m_CubeModel, 1, GL_FALSE, Model);
	glUniformMatrix4fv (m_CubeProj, 1, GL_FALSE, Proj);

	glBindBuffer (GL_ARRAY_BUFFER, m_CubeBuffer);
	const GLsizei nStride = 9 * sizeof (GLfloat);
	glVertexAttribPointer (0, 3, GL_FLOAT, GL_FALSE, nStride, (void *) 0);
	glVertexAttribPointer (1, 3, GL_FLOAT, GL_FALSE, nStride, (void *) (3 * sizeof (GLfloat)));
	glVertexAttribPointer (2, 3, GL_FLOAT, GL_FALSE, nStride, (void *) (6 * sizeof (GLfloat)));
	glEnableVertexAttribArray (0);
	glEnableVertexAttribArray (1);
	glEnableVertexAttribArray (2);
	glDrawArrays (GL_TRIANGLES, 0, 36);
	glDisableVertexAttribArray (0);
	glDisableVertexAttribArray (1);
	glDisableVertexAttribArray (2);
}

void CRenderer::Finish (void)
{
	glFinish ();
}

unsigned CRenderer::ReadPixelsRGB565BE (u16 *pBuffer)
{
	assert (pBuffer != 0);
	assert (m_pReadBuffer != 0);

	unsigned nStart = CTimer::GetClockTicks ();
	glReadPixels (0, 0, m_nWidth, m_nHeight, GL_RGBA, GL_UNSIGNED_BYTE, m_pReadBuffer);
	unsigned nReadUs = CTimer::GetClockTicks () - nStart;

	// GL rows start at the bottom, the display's at the top
	for (unsigned y = 0; y < m_nHeight; y++)
	{
		const u8 *pSrc = m_pReadBuffer + (m_nHeight-1 - y) * m_nWidth * 4;
		u16 *pDst = pBuffer + y * m_nWidth;

		for (unsigned x = 0; x < m_nWidth; x++, pSrc += 4)
		{
			u16 usColor = (pSrc[0] >> 3) << 11 | (pSrc[1] >> 2) << 5 | pSrc[2] >> 3;
			*pDst++ = bswap16 (usColor);
		}
	}

	return nReadUs;
}

GLuint CRenderer::CompileShader (GLenum Type, const char *pSource)
{
	GLuint Shader = glCreateShader (Type);
	glShaderSource (Shader, 1, &pSource, nullptr);
	glCompileShader (Shader);

	GLint nStatus;
	glGetShaderiv (Shader, GL_COMPILE_STATUS, &nStatus);
	if (!nStatus)
	{
		char Log[256];
		glGetShaderInfoLog (Shader, sizeof Log, nullptr, Log);
		LOGERR ("Shader compile failed: %s", Log);
		return 0;
	}

	return Shader;
}

GLuint CRenderer::LinkProgram (const char *pVertex, const char *pFragment)
{
	GLuint VertexShader = CompileShader (GL_VERTEX_SHADER, pVertex);
	GLuint FragmentShader = CompileShader (GL_FRAGMENT_SHADER, pFragment);
	if (!VertexShader || !FragmentShader)
	{
		return 0;
	}

	GLuint Program = glCreateProgram ();
	glAttachShader (Program, VertexShader);
	glAttachShader (Program, FragmentShader);
	glBindAttribLocation (Program, 0, "a_pos");
	glBindAttribLocation (Program, 1, "a_normal");
	glBindAttribLocation (Program, 2, "a_color");
	glLinkProgram (Program);

	GLint nStatus;
	glGetProgramiv (Program, GL_LINK_STATUS, &nStatus);
	if (!nStatus)
	{
		char Log[256];
		glGetProgramInfoLog (Program, sizeof Log, nullptr, Log);
		LOGERR ("Program link failed: %s", Log);
		return 0;
	}

	return Program;
}

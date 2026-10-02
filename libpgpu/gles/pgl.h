/*
 * pgl.h - an OpenGL ES 2.0 API (with the GL ES 1.1 fixed-function calls) for
 * piegpu, on top of pgpu (docs/protocol.md).
 *
 * The GL state lives here (for glGet*, glIsEnabled, name allocation); every
 * call is encoded into the command stream. Differences to a desktop driver:
 *
 * - There is no shader compiler (GL_SHADER_COMPILER is GL_FALSE). Programs
 *   are precompiled on the PC by tools/glslc and loaded, as GL ES 2.0 allows,
 *   with glShaderBinary (2, {vs, fs}, PGL_SHADER_BINARY_PGPU, &NAME_info,
 *   sizeof NAME_info) and glLinkProgram, or with glProgramBinaryOES (program,
 *   PGL_PROGRAM_BINARY_PGPU, &NAME_info, sizeof NAME_info). NAME_info is
 *   defined by the generated header. Attribute locations are those glslc was
 *   given (-a order).
 * - glGetError returns errors found here at once; errors the RPi reports come
 *   in asynchronously (after glFlush, when the RPi has executed the command).
 *   After glFinish, all errors of earlier commands are there.
 * - The default framebuffer is the panel: RGB565, 24-bit depth, 8-bit stencil.
 *   Framebuffer objects need a colour attachment (a texture or an RGBA4,
 *   RGB565, RGB5_A1 or RGBA8_OES renderbuffer); a depth and/or stencil
 *   renderbuffer gives them a (combined) depth and stencil buffer.
 * - The fixed-function pipeline (GL ES 1.1) is used while program 0 is current.
 *   Its client arrays must all be client-side or all in buffers.
 */
#ifndef PGL_H
#define PGL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "pgl_enums.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef unsigned int	GLenum;
typedef unsigned char	GLboolean;
typedef unsigned int	GLbitfield;
typedef void		GLvoid;
typedef int8_t		GLbyte;
typedef int16_t		GLshort;
typedef int32_t		GLint;
typedef int32_t		GLsizei;
typedef uint8_t		GLubyte;
typedef uint16_t	GLushort;
typedef uint32_t	GLuint;
typedef float		GLfloat;
typedef float		GLclampf;
typedef int32_t		GLfixed;
typedef intptr_t	GLintptr;
typedef intptr_t	GLsizeiptr;
typedef char		GLchar;

/* glProgramBinaryOES and glShaderBinary formats: a pgpu_program_info_t from
   tools/glslc (NAME_info); for glShaderBinary, load it into the vertex and
   the fragment shader, then glLinkProgram */
#define PGL_PROGRAM_BINARY_PGPU		0x9A50
#define PGL_SHADER_BINARY_PGPU		0x9A51

/* ---- pgl ---------------------------------------------------------------------- */

/* after pgpu_init (): RESET the RPi, set the GL defaults; false if the RPi
   doesn't answer */
bool pglInit (void);
/* pglInit, and the default framebuffer offscreen instead of the panel: a
   WIDTH x HEIGHT RGBA8888 surface with 24-bit depth and 8-bit stencil (a
   pbuffer: glReadPixels reads it, the panel is not drawn) */
bool pglInitSurface (unsigned width, unsigned height);
/* end the frame: the screen shows it (FRAME_END). Picks up a new screen size
   (pglGetScreenSize) */
void pglSwapBuffers (void);
/* video: texture (a GL texture name) becomes a video texture of stream (1 or
   2): RGBA, width x height (width a power of two, 32 or more; height a
   multiple of 16), linear, clamped, one level; the RPi's decoder puts H.264
   of coded_width x coded_height into it, scaled, and it shows the frame that's
   due at each frame (docs/protocol.md 7.12). avcc: the samples are as MP4 has
   them (pgpu_mp4's avcC); NULL: Annex B. Feed it with pgpu_media_sample or
   pgpu_media_sample_read (pgpu.h); defining the texture again (glTexImage2D)
   ends that. False on a bad name */
bool pglVideoTexture (GLuint texture, unsigned stream, unsigned width, unsigned height,
		      unsigned coded_width, unsigned coded_height, const void *avcc, unsigned avcc_bytes);
/* a video texture (of stream) gets another size; the video goes on (e.g. the
   screen changed). False on a bad name */
bool pglVideoResize (GLuint texture, unsigned stream, unsigned width, unsigned height);

/* the screen's size now: the panel's, or on HDMI one chosen for the monitor
   (it changes when a monitor is plugged in or out: pgpu_get_display). A
   viewport and scissor box covering the whole screen follow it */
void pglGetScreenSize (unsigned *width, unsigned *height);
/* antialiasing by the RPi: 4 samples a pixel (1: none, as at the start) for
   what is drawn from now on, the screen or a texture; they are averaged as
   the picture is stored, so nothing else changes (glReadPixels, textures and
   GL_SAMPLES see single samples). The RPi renders what was drawn so far when
   it changes: once a frame is fine, between every two draws is not. Blending
   takes each sample's colour in programs compiled for it (glslc --ms);
   in others the pixel's samples blend with the first one's */
void pglSamples (unsigned samples);
/* the last ERROR reply of the RPi (code, opcode, detail; docs/protocol.md 9),
   for finding out what a GL error from the RPi was about */
void pglGetRPiError (uint32_t error[3]);

/* ---- GL ES 2.0 ---------------------------------------------------------------- */

void glActiveTexture (GLenum texture);
void glAttachShader (GLuint program, GLuint shader);
void glBindAttribLocation (GLuint program, GLuint index, const GLchar *name);
void glBindBuffer (GLenum target, GLuint buffer);
void glBindFramebuffer (GLenum target, GLuint framebuffer);
void glBindRenderbuffer (GLenum target, GLuint renderbuffer);
void glBindTexture (GLenum target, GLuint texture);
void glBlendColor (GLclampf red, GLclampf green, GLclampf blue, GLclampf alpha);
void glBlendEquation (GLenum mode);
void glBlendEquationSeparate (GLenum modeRGB, GLenum modeAlpha);
void glBlendFunc (GLenum sfactor, GLenum dfactor);
void glBlendFuncSeparate (GLenum srcRGB, GLenum dstRGB, GLenum srcAlpha, GLenum dstAlpha);
void glBufferData (GLenum target, GLsizeiptr size, const void *data, GLenum usage);
void glBufferSubData (GLenum target, GLintptr offset, GLsizeiptr size, const void *data);
GLenum glCheckFramebufferStatus (GLenum target);
void glClear (GLbitfield mask);
void glClearColor (GLclampf red, GLclampf green, GLclampf blue, GLclampf alpha);
void glClearDepthf (GLclampf depth);
void glClearStencil (GLint s);
void glColorMask (GLboolean red, GLboolean green, GLboolean blue, GLboolean alpha);
void glCompileShader (GLuint shader);
void glCompressedTexImage2D (GLenum target, GLint level, GLenum internalformat, GLsizei width,
			     GLsizei height, GLint border, GLsizei imageSize, const void *data);
void glCompressedTexSubImage2D (GLenum target, GLint level, GLint xoffset, GLint yoffset,
				GLsizei width, GLsizei height, GLenum format, GLsizei imageSize,
				const void *data);
void glCopyTexImage2D (GLenum target, GLint level, GLenum internalformat, GLint x, GLint y,
		       GLsizei width, GLsizei height, GLint border);
void glCopyTexSubImage2D (GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint x,
			  GLint y, GLsizei width, GLsizei height);
GLuint glCreateProgram (void);
GLuint glCreateShader (GLenum type);
void glCullFace (GLenum mode);
void glDeleteBuffers (GLsizei n, const GLuint *buffers);
void glDeleteFramebuffers (GLsizei n, const GLuint *framebuffers);
void glDeleteProgram (GLuint program);
void glDeleteRenderbuffers (GLsizei n, const GLuint *renderbuffers);
void glDeleteShader (GLuint shader);
void glDeleteTextures (GLsizei n, const GLuint *textures);
void glDepthFunc (GLenum func);
void glDepthMask (GLboolean flag);
void glDepthRangef (GLclampf zNear, GLclampf zFar);
void glDetachShader (GLuint program, GLuint shader);
void glDisable (GLenum cap);
void glDisableVertexAttribArray (GLuint index);
void glDrawArrays (GLenum mode, GLint first, GLsizei count);
void glDrawElements (GLenum mode, GLsizei count, GLenum type, const void *indices);
void glEnable (GLenum cap);
void glEnableVertexAttribArray (GLuint index);
void glFinish (void);
void glFlush (void);
void glFramebufferRenderbuffer (GLenum target, GLenum attachment, GLenum renderbuffertarget,
				GLuint renderbuffer);
void glFramebufferTexture2D (GLenum target, GLenum attachment, GLenum textarget, GLuint texture,
			     GLint level);
void glFrontFace (GLenum mode);
void glGenBuffers (GLsizei n, GLuint *buffers);
void glGenerateMipmap (GLenum target);
void glGenFramebuffers (GLsizei n, GLuint *framebuffers);
void glGenRenderbuffers (GLsizei n, GLuint *renderbuffers);
void glGenTextures (GLsizei n, GLuint *textures);
void glGetActiveAttrib (GLuint program, GLuint index, GLsizei bufsize, GLsizei *length, GLint *size,
			GLenum *type, GLchar *name);
void glGetActiveUniform (GLuint program, GLuint index, GLsizei bufsize, GLsizei *length, GLint *size,
			 GLenum *type, GLchar *name);
void glGetAttachedShaders (GLuint program, GLsizei maxcount, GLsizei *count, GLuint *shaders);
GLint glGetAttribLocation (GLuint program, const GLchar *name);
void glGetBooleanv (GLenum pname, GLboolean *params);
void glGetBufferParameteriv (GLenum target, GLenum pname, GLint *params);
GLenum glGetError (void);
void glGetFloatv (GLenum pname, GLfloat *params);
void glGetFramebufferAttachmentParameteriv (GLenum target, GLenum attachment, GLenum pname, GLint *params);
void glGetIntegerv (GLenum pname, GLint *params);
void glGetProgramiv (GLuint program, GLenum pname, GLint *params);
void glGetProgramInfoLog (GLuint program, GLsizei bufsize, GLsizei *length, GLchar *infolog);
void glGetRenderbufferParameteriv (GLenum target, GLenum pname, GLint *params);
void glGetShaderiv (GLuint shader, GLenum pname, GLint *params);
void glGetShaderInfoLog (GLuint shader, GLsizei bufsize, GLsizei *length, GLchar *infolog);
void glGetShaderPrecisionFormat (GLenum shadertype, GLenum precisiontype, GLint *range, GLint *precision);
void glGetShaderSource (GLuint shader, GLsizei bufsize, GLsizei *length, GLchar *source);
const GLubyte *glGetString (GLenum name);
void glGetTexParameterfv (GLenum target, GLenum pname, GLfloat *params);
void glGetTexParameteriv (GLenum target, GLenum pname, GLint *params);
void glGetUniformfv (GLuint program, GLint location, GLfloat *params);
void glGetUniformiv (GLuint program, GLint location, GLint *params);
GLint glGetUniformLocation (GLuint program, const GLchar *name);
void glGetVertexAttribfv (GLuint index, GLenum pname, GLfloat *params);
void glGetVertexAttribiv (GLuint index, GLenum pname, GLint *params);
void glGetVertexAttribPointerv (GLuint index, GLenum pname, void **pointer);
void glHint (GLenum target, GLenum mode);
GLboolean glIsBuffer (GLuint buffer);
GLboolean glIsEnabled (GLenum cap);
GLboolean glIsFramebuffer (GLuint framebuffer);
GLboolean glIsProgram (GLuint program);
GLboolean glIsRenderbuffer (GLuint renderbuffer);
GLboolean glIsShader (GLuint shader);
GLboolean glIsTexture (GLuint texture);
void glLineWidth (GLfloat width);
void glLinkProgram (GLuint program);
void glPixelStorei (GLenum pname, GLint param);
void glPolygonOffset (GLfloat factor, GLfloat units);
void glReadPixels (GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, void *pixels);
void glReleaseShaderCompiler (void);
void glRenderbufferStorage (GLenum target, GLenum internalformat, GLsizei width, GLsizei height);
void glSampleCoverage (GLclampf value, GLboolean invert);
void glScissor (GLint x, GLint y, GLsizei width, GLsizei height);
void glShaderBinary (GLsizei n, const GLuint *shaders, GLenum binaryformat, const void *binary, GLsizei length);
void glShaderSource (GLuint shader, GLsizei count, const GLchar *const *string, const GLint *length);
void glStencilFunc (GLenum func, GLint ref, GLuint mask);
void glStencilFuncSeparate (GLenum face, GLenum func, GLint ref, GLuint mask);
void glStencilMask (GLuint mask);
void glStencilMaskSeparate (GLenum face, GLuint mask);
void glStencilOp (GLenum fail, GLenum zfail, GLenum zpass);
void glStencilOpSeparate (GLenum face, GLenum fail, GLenum zfail, GLenum zpass);
void glTexImage2D (GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height,
		   GLint border, GLenum format, GLenum type, const void *pixels);
void glTexParameterf (GLenum target, GLenum pname, GLfloat param);
void glTexParameterfv (GLenum target, GLenum pname, const GLfloat *params);
void glTexParameteri (GLenum target, GLenum pname, GLint param);
void glTexParameteriv (GLenum target, GLenum pname, const GLint *params);
void glTexSubImage2D (GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width,
		      GLsizei height, GLenum format, GLenum type, const void *pixels);
void glUniform1f (GLint location, GLfloat x);
void glUniform1fv (GLint location, GLsizei count, const GLfloat *v);
void glUniform1i (GLint location, GLint x);
void glUniform1iv (GLint location, GLsizei count, const GLint *v);
void glUniform2f (GLint location, GLfloat x, GLfloat y);
void glUniform2fv (GLint location, GLsizei count, const GLfloat *v);
void glUniform2i (GLint location, GLint x, GLint y);
void glUniform2iv (GLint location, GLsizei count, const GLint *v);
void glUniform3f (GLint location, GLfloat x, GLfloat y, GLfloat z);
void glUniform3fv (GLint location, GLsizei count, const GLfloat *v);
void glUniform3i (GLint location, GLint x, GLint y, GLint z);
void glUniform3iv (GLint location, GLsizei count, const GLint *v);
void glUniform4f (GLint location, GLfloat x, GLfloat y, GLfloat z, GLfloat w);
void glUniform4fv (GLint location, GLsizei count, const GLfloat *v);
void glUniform4i (GLint location, GLint x, GLint y, GLint z, GLint w);
void glUniform4iv (GLint location, GLsizei count, const GLint *v);
void glUniformMatrix2fv (GLint location, GLsizei count, GLboolean transpose, const GLfloat *value);
void glUniformMatrix3fv (GLint location, GLsizei count, GLboolean transpose, const GLfloat *value);
void glUniformMatrix4fv (GLint location, GLsizei count, GLboolean transpose, const GLfloat *value);
void glUseProgram (GLuint program);
void glValidateProgram (GLuint program);
void glVertexAttrib1f (GLuint indx, GLfloat x);
void glVertexAttrib1fv (GLuint indx, const GLfloat *values);
void glVertexAttrib2f (GLuint indx, GLfloat x, GLfloat y);
void glVertexAttrib2fv (GLuint indx, const GLfloat *values);
void glVertexAttrib3f (GLuint indx, GLfloat x, GLfloat y, GLfloat z);
void glVertexAttrib3fv (GLuint indx, const GLfloat *values);
void glVertexAttrib4f (GLuint indx, GLfloat x, GLfloat y, GLfloat z, GLfloat w);
void glVertexAttrib4fv (GLuint indx, const GLfloat *values);
void glVertexAttribPointer (GLuint indx, GLint size, GLenum type, GLboolean normalized, GLsizei stride,
			    const void *ptr);
void glViewport (GLint x, GLint y, GLsizei width, GLsizei height);

/* OES_get_program_binary */
void glGetProgramBinaryOES (GLuint program, GLsizei bufSize, GLsizei *length, GLenum *binaryFormat,
			    void *binary);
void glProgramBinaryOES (GLuint program, GLenum binaryFormat, const void *binary, GLint length);
/* GL_EXT_debug_marker: markers for a debugger; nothing records them here */
void glInsertEventMarkerEXT (GLsizei length, const GLchar *marker);
void glPushGroupMarkerEXT (GLsizei length, const GLchar *marker);
void glPopGroupMarkerEXT (void);

/* ---- GL ES 1.1 fixed function (used while program 0 is current) --------------- */

void glAlphaFunc (GLenum func, GLclampf ref);
void glClientActiveTexture (GLenum texture);
void glColor4f (GLfloat red, GLfloat green, GLfloat blue, GLfloat alpha);
void glColor4ub (GLubyte red, GLubyte green, GLubyte blue, GLubyte alpha);
void glColorPointer (GLint size, GLenum type, GLsizei stride, const void *pointer);
void glDisableClientState (GLenum array);
void glEnableClientState (GLenum array);
void glFogf (GLenum pname, GLfloat param);
void glFogfv (GLenum pname, const GLfloat *params);
void glFrustumf (GLfloat left, GLfloat right, GLfloat bottom, GLfloat top, GLfloat zNear, GLfloat zFar);
void glGetLightfv (GLenum light, GLenum pname, GLfloat *params);
void glGetMaterialfv (GLenum face, GLenum pname, GLfloat *params);
void glGetTexEnvfv (GLenum env, GLenum pname, GLfloat *params);
void glLightf (GLenum light, GLenum pname, GLfloat param);
void glLightfv (GLenum light, GLenum pname, const GLfloat *params);
void glLightModelf (GLenum pname, GLfloat param);
void glLightModelfv (GLenum pname, const GLfloat *params);
void glLoadIdentity (void);
void glLoadMatrixf (const GLfloat *m);
void glMaterialf (GLenum face, GLenum pname, GLfloat param);
void glMaterialfv (GLenum face, GLenum pname, const GLfloat *params);
void glMatrixMode (GLenum mode);
void glMultMatrixf (const GLfloat *m);
void glMultiTexCoord4f (GLenum target, GLfloat s, GLfloat t, GLfloat r, GLfloat q);
void glNormal3f (GLfloat nx, GLfloat ny, GLfloat nz);
void glNormalPointer (GLenum type, GLsizei stride, const void *pointer);
void glOrthof (GLfloat left, GLfloat right, GLfloat bottom, GLfloat top, GLfloat zNear, GLfloat zFar);
void glPopMatrix (void);
void glPushMatrix (void);
void glRotatef (GLfloat angle, GLfloat x, GLfloat y, GLfloat z);
void glScalef (GLfloat x, GLfloat y, GLfloat z);
void glShadeModel (GLenum mode);
void glTexCoordPointer (GLint size, GLenum type, GLsizei stride, const void *pointer);
void glTexEnvf (GLenum target, GLenum pname, GLfloat param);
void glTexEnvfv (GLenum target, GLenum pname, const GLfloat *params);
void glTexEnvi (GLenum target, GLenum pname, GLint param);
void glTranslatef (GLfloat x, GLfloat y, GLfloat z);
void glVertexPointer (GLint size, GLenum type, GLsizei stride, const void *pointer);

#ifdef __cplusplus
}
#endif

#endif

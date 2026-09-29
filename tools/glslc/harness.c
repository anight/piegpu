/*
 * harness.c - drives Mesa's vc4 driver (on the host, through its no-hardware
 * DRM shim) to compile a GLSL ES 1.00 program for the VideoCore IV.
 *
 * Run by glslc.py, which sets LD_PRELOAD, MESA_LOADER_DRIVER_OVERRIDE=vc4 and
 * PGPU_VC4_DUMP (the patched driver appends each compiled shader used by a
 * draw to that file). The job file lists:
 *
 *	vs <file>
 *	fs <file>
 *	attrib <name> <float|byte|ubyte|short|ushort> <size 1-4> <normalized 0|1>
 *	variant <triangles|lines|points|points_texture>
 *	check		(only compile the shaders listed, print the info log)
 *	probe		(link without attribute locations, print the attributes)
 *
 * Attributes get locations 0, 1, ... in the order listed; "a,b" are names
 * bound to one location (aliases), "-" is a location no attribute starts at. Every active
 * uniform scalar is set to a unique marker value so that glslc.py can find
 * where it lands in the uniform streams; sampler n is given texture unit n
 * with a texture of width 4 << n (visible in the texture config uniforms).
 *
 * stdout: "uniform", "marker", "sampler" and "error" lines for glslc.py.
 */
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MARKER_BASE	1000000		/* marker k: (MARKER_BASE + k), as a float or an int
					   (bools can't hold markers: Mesa stores true as ~0) */

static char *read_file (const char *path)
{
	FILE *f = fopen (path, "rb");
	if (!f)
	{
		printf ("error cannot open %s\n", path);
		exit (1);
	}
	fseek (f, 0, SEEK_END);
	long n = ftell (f);
	fseek (f, 0, SEEK_SET);
	char *s = malloc (n + 1);
	if (fread (s, 1, n, f) != (size_t) n)
	{
		printf ("error cannot read %s\n", path);
		exit (1);
	}
	s[n] = 0;
	fclose (f);
	return s;
}

static GLuint compile (GLenum type, const char *path)
{
	char *src = read_file (path);
	GLuint h = glCreateShader (type);
	glShaderSource (h, 1, (const char **) &src, NULL);
	glCompileShader (h);
	GLint ok;
	glGetShaderiv (h, GL_COMPILE_STATUS, &ok);
	if (!ok)
	{
		char log[8192];
		glGetShaderInfoLog (h, sizeof log, NULL, log);
		for (char *l = strtok (log, "\n"); l; l = strtok (NULL, "\n"))
			printf ("error %s: %s\n", path, l);
		exit (1);
	}
	free (src);
	return h;
}

static int components (GLenum type, int *is_int)
{
	*is_int = 0;
	switch (type)
	{
	case GL_FLOAT:		return 1;
	case GL_FLOAT_VEC2:	return 2;
	case GL_FLOAT_VEC3:	return 3;
	case GL_FLOAT_VEC4:	return 4;
	case GL_FLOAT_MAT2:	return 4;
	case GL_FLOAT_MAT3:	return 9;
	case GL_FLOAT_MAT4:	return 16;
	case GL_INT: case GL_BOOL:		*is_int = 1; return 1;
	case GL_INT_VEC2: case GL_BOOL_VEC2:	*is_int = 1; return 2;
	case GL_INT_VEC3: case GL_BOOL_VEC3:	*is_int = 1; return 3;
	case GL_INT_VEC4: case GL_BOOL_VEC4:	*is_int = 1; return 4;
	default:		return 0;	/* samplers */
	}
}

static void set_uniform (GLint loc, GLenum type, const float *f, const int *i)
{
	switch (type)
	{
	case GL_FLOAT:		glUniform1fv (loc, 1, f); break;
	case GL_FLOAT_VEC2:	glUniform2fv (loc, 1, f); break;
	case GL_FLOAT_VEC3:	glUniform3fv (loc, 1, f); break;
	case GL_FLOAT_VEC4:	glUniform4fv (loc, 1, f); break;
	case GL_FLOAT_MAT2:	glUniformMatrix2fv (loc, 1, GL_FALSE, f); break;
	case GL_FLOAT_MAT3:	glUniformMatrix3fv (loc, 1, GL_FALSE, f); break;
	case GL_FLOAT_MAT4:	glUniformMatrix4fv (loc, 1, GL_FALSE, f); break;
	case GL_INT: case GL_BOOL:		glUniform1iv (loc, 1, i); break;
	case GL_INT_VEC2: case GL_BOOL_VEC2:	glUniform2iv (loc, 1, i); break;
	case GL_INT_VEC3: case GL_BOOL_VEC3:	glUniform3iv (loc, 1, i); break;
	case GL_INT_VEC4: case GL_BOOL_VEC4:	glUniform4iv (loc, 1, i); break;
	}
}

int main (int argc, char **argv)
{
	if (argc != 3)
	{
		fprintf (stderr, "usage: harness <job file> <dump file>\n");
		return 2;
	}

	char vs[1024] = "", fs[1024] = "";
	struct { char name[128]; GLenum type; int size, norm; } attribs[8];
	int nattribs = 0;
	struct { GLenum prim; char blend[32]; int texture_target; } variants[32];
	int nvariants = 0;
	int check = 0, probe = 0;

	FILE *job = fopen (argv[1], "r");
	if (!job)
	{
		printf ("error cannot open job file\n");
		return 1;
	}
	char line[2048];
	while (fgets (line, sizeof line, job))
	{
		char a[1024], b[64];
		int n, m;
		if (!strncmp (line, "check", 5))
			check = 1;
		else if (!strncmp (line, "probe", 5))
			probe = 1;
		else if (sscanf (line, "vs %1023s", a) == 1)
			strcpy (vs, a);
		else if (sscanf (line, "fs %1023s", a) == 1)
			strcpy (fs, a);
		else if (sscanf (line, "attrib %127s %63s %d %d", a, b, &n, &m) == 4 && nattribs < 8)
		{
			strcpy (attribs[nattribs].name, a);
			attribs[nattribs].type =   !strcmp (b, "float") ? GL_FLOAT
						 : !strcmp (b, "fixed") ? GL_FIXED
						 : !strcmp (b, "byte") ? GL_BYTE
						 : !strcmp (b, "ubyte") ? GL_UNSIGNED_BYTE
						 : !strcmp (b, "short") ? GL_SHORT : GL_UNSIGNED_SHORT;
			attribs[nattribs].size = n;
			attribs[nattribs].norm = m;
			nattribs++;
		}
		else if (sscanf (line, "variant %63s", b) == 1 && nvariants < 32)
		{
			variants[nvariants].prim =   !strncmp (b, "points", 6) ? GL_POINTS
						   : !strcmp (b, "lines") ? GL_LINES : GL_TRIANGLES;
			strcpy (variants[nvariants].blend, "none");
			variants[nvariants].texture_target = !strcmp (b, "points_texture");
			nvariants++;
		}
	}
	fclose (job);

	/* EGL on the surfaceless platform: the vc4 driver on the DRM shim */
	PFNEGLGETPLATFORMDISPLAYEXTPROC get_display =
		(PFNEGLGETPLATFORMDISPLAYEXTPROC) eglGetProcAddress ("eglGetPlatformDisplayEXT");
	EGLDisplay dpy = get_display (EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
	if (!eglInitialize (dpy, NULL, NULL))
	{
		printf ("error eglInitialize failed\n");
		return 1;
	}
	eglBindAPI (EGL_OPENGL_ES_API);

	/* render target: an 8888 pbuffer with depth and stencil. A window-system
	   framebuffer like the RPi's panel (rows top down, so Mesa flips
	   gl_FragCoord, gl_PointCoord and facing as for a window), with alpha:
	   the fragment shader keeps the source alpha, which glslc's blend code
	   needs. Mesa's colour output is BGRA (B in byte 0); glslc reorders it. */
	EGLint config_attribs[] =
	{
		EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
		EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
		EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
		EGL_DEPTH_SIZE, 24, EGL_STENCIL_SIZE, 8,
		EGL_NONE
	};
	EGLConfig configs[64];
	EGLint nconfigs = 0;
	eglChooseConfig (dpy, config_attribs, configs, 64, &nconfigs);
	EGLConfig config = NULL;
	for (int i = 0; i < nconfigs && !config; i++)
	{
		EGLint r, g, b, a;
		eglGetConfigAttrib (dpy, configs[i], EGL_RED_SIZE, &r);
		eglGetConfigAttrib (dpy, configs[i], EGL_GREEN_SIZE, &g);
		eglGetConfigAttrib (dpy, configs[i], EGL_BLUE_SIZE, &b);
		eglGetConfigAttrib (dpy, configs[i], EGL_ALPHA_SIZE, &a);
		if (r == 8 && g == 8 && b == 8 && a == 8)
			config = configs[i];
	}
	if (!config)
	{
		printf ("error no RGBA8888 pbuffer config\n");
		return 1;
	}
	EGLint pbuffer_attribs[] = {EGL_WIDTH, 320, EGL_HEIGHT, 240, EGL_NONE};
	EGLSurface surface = eglCreatePbufferSurface (dpy, config, pbuffer_attribs);
	EGLint ctx_attribs[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
	EGLContext ctx = eglCreateContext (dpy, config, EGL_NO_CONTEXT, ctx_attribs);
	if (surface == EGL_NO_SURFACE || !eglMakeCurrent (dpy, surface, surface, ctx))
	{
		printf ("error cannot make the pbuffer current\n");
		return 1;
	}
	const char *renderer = (const char *) glGetString (GL_RENDERER);
	if (!renderer || !strstr (renderer, "VC4"))
	{
		printf ("error not the vc4 driver: %s\n", renderer ? renderer : "?");
		return 1;
	}

	if (check)
	{
		/* compile errors are printed by compile () */
		if (vs[0])
			compile (GL_VERTEX_SHADER, vs);
		if (fs[0])
			compile (GL_FRAGMENT_SHADER, fs);
		printf ("compiled\n");
		return 0;
	}

	glViewport (0, 0, 320, 240);
	glEnable (GL_DEPTH_TEST);

	/* stencil test on, two-sided with uncommon write masks: the fragment
	   shader then writes all three stencil setup words from uniforms (the
	   RPi sets "always pass, keep" while the test is off) */
	glEnable (GL_STENCIL_TEST);
	glStencilFuncSeparate (GL_FRONT, GL_LESS, 1, 0x5A);
	glStencilFuncSeparate (GL_BACK, GL_GREATER, 2, 0x3C);
	glStencilOpSeparate (GL_FRONT, GL_INCR, GL_DECR, GL_REPLACE);
	glStencilOpSeparate (GL_BACK, GL_DECR, GL_INCR, GL_INVERT);
	glStencilMaskSeparate (GL_FRONT, 0x5A);
	glStencilMaskSeparate (GL_BACK, 0x3C);

	GLuint prog = glCreateProgram ();
	glAttachShader (prog, compile (GL_VERTEX_SHADER, vs));
	glAttachShader (prog, compile (GL_FRAGMENT_SHADER, fs));
	for (int i = 0; i < nattribs; i++)
	{
		/* "a,b": aliases at one location; "-": no attribute starts here */
		char names[128];
		strcpy (names, attribs[i].name);
		for (char *n = strtok (names, ","); n; n = strtok (NULL, ","))
			if (strcmp (n, "-"))
				glBindAttribLocation (prog, i, n);
	}
	glLinkProgram (prog);
	GLint ok;
	glGetProgramiv (prog, GL_LINK_STATUS, &ok);
	if (!ok)
	{
		char log[8192];
		glGetProgramInfoLog (prog, sizeof log, NULL, log);
		for (char *l = strtok (log, "\n"); l; l = strtok (NULL, "\n"))
			printf ("error link: %s\n", l);
		return 1;
	}
	glUseProgram (prog);

	/* attributes that the program has but the job didn't list */
	GLint nactive;
	glGetProgramiv (prog, GL_ACTIVE_ATTRIBUTES, &nactive);
	for (int i = 0; i < nactive; i++)
	{
		char name[256];
		GLint size;
		GLenum type;
		glGetActiveAttrib (prog, i, sizeof name, NULL, &size, &type, name);
		int found = 0;
		for (int k = 0; k < nattribs; k++)
		{
			char names[128];
			strcpy (names, attribs[k].name);
			for (char *n = strtok (names, ","); n; n = strtok (NULL, ","))
				found |= !strcmp (n, name);
		}
		printf ("attribute %s 0x%04x %d\n", name, type, size);
		if (!found && !probe)
		{
			printf ("error attribute %s is not listed in the program description\n", name);
			return 1;
		}
	}

	if (probe)
	{
		return 0;
	}

	/* uniforms: markers; samplers: unit n, texture width 4 << n */
	glGetProgramiv (prog, GL_ACTIVE_UNIFORMS, &nactive);
	int marker = 0, sampler = 0;
	for (int u = 0; u < nactive; u++)
	{
		char name[256];
		GLint size;
		GLenum type;
		glGetActiveUniform (prog, u, sizeof name, NULL, &size, &type, name);
		/* arrays are reported as "name[0]" (only the last subscript: members
		   of arrays of structures are "name[1].member") */
		size_t len = strlen (name);
		int is_array = len > 3 && !strcmp (name + len - 3, "[0]");
		if (is_array)
			name[len - 3] = 0;

		int is_int, n = components (type, &is_int);
		printf ("uniform %d %s 0x%04x %d %d\n", u, name, type, size, is_array);

		if (type == GL_SAMPLER_2D || type == GL_SAMPLER_CUBE)
		{
			/* every element of a sampler array its own unit (and texture
			   width): glslc tells them apart; element 0 carries the name */
			GLint loc = glGetUniformLocation (prog, name);
			GLint units[32];
			for (int e = 0; e < size && e < 32; e++)
				units[e] = sampler + e;
			glUniform1iv (loc, size < 32 ? size : 32, units);
			for (int e = 0; e < size && e < 32; e++, sampler++)
			{
				glActiveTexture (GL_TEXTURE0 + sampler);
				GLuint tex;
				glGenTextures (1, &tex);
				if (type == GL_SAMPLER_2D)
				{
					glBindTexture (GL_TEXTURE_2D, tex);
					glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA, 4 << sampler, 4, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
					glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
					glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
					glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
				}
				else
				{
					glBindTexture (GL_TEXTURE_CUBE_MAP, tex);
					for (int f = 0; f < 6; f++)
						glTexImage2D (GL_TEXTURE_CUBE_MAP_POSITIVE_X + f, 0, GL_RGBA, 4 << sampler,
							      4 << sampler, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
					glTexParameteri (GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
					glTexParameteri (GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
					glTexParameteri (GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
				}
				if (e == 0)
					printf ("sampler %s %d %s\n", name, sampler, type == GL_SAMPLER_CUBE ? "cube" : "2d");
				else
					printf ("sampler %s[%d] %d %s\n", name, e, sampler,
						type == GL_SAMPLER_CUBE ? "cube" : "2d");
			}
			continue;
		}
		if (n == 0)
		{
			printf ("error %s: unsupported uniform type 0x%04x\n", name, type);
			return 1;
		}

		for (int e = 0; e < size; e++)
		{
			char elem[300];
			if (size > 1)
				snprintf (elem, sizeof elem, "%s[%d]", name, e);
			else
				snprintf (elem, sizeof elem, "%s", name);
			GLint loc = glGetUniformLocation (prog, elem);
			float f[16];
			int iv[16];
			for (int c = 0; c < n; c++)
			{
				printf ("marker %d %d %d %d\n", marker, u, e, c);
				f[c] = (float) (MARKER_BASE + marker);
				iv[c] = MARKER_BASE + marker;
				marker++;
			}
			set_uniform (loc, type, f, iv);
		}
	}
	glActiveTexture (GL_TEXTURE0);

	/* vertex data for the draws: enough bytes for any format */
	static float data[64 * 4];
	for (int i = 0; i < nattribs; i++)
	{
		glVertexAttribPointer (i, attribs[i].size, attribs[i].type, attribs[i].norm, 0, data);
		glEnableVertexAttribArray (i);
	}

	FILE *dump = fopen (argv[2], "a");
	for (int v = 0; v < nvariants; v++)
	{
		const char *b = variants[v].blend;
		if (!strcmp (b, "none"))
			glDisable (GL_BLEND);
		else
		{
			glEnable (GL_BLEND);
			glBlendEquation (GL_FUNC_ADD);
			if (!strcmp (b, "alpha"))
				glBlendFunc (GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
			else if (!strcmp (b, "add"))
				glBlendFunc (GL_ONE, GL_ONE);
			else if (!strcmp (b, "premul"))
				glBlendFunc (GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
			else if (!strcmp (b, "multiply"))
				glBlendFunc (GL_DST_COLOR, GL_ZERO);
			else
			{
				printf ("error unknown blend mode %s\n", b);
				return 1;
			}
		}

		fprintf (dump, "{\"variant\": %d}\n", v);
		fflush (dump);
		if (variants[v].texture_target)
			setenv ("PGPU_POINT_LOWER_LEFT", "1", 1);	/* see the Mesa patch */
		glDrawArrays (variants[v].prim, 0, 6);
		glFlush ();
		unsetenv ("PGPU_POINT_LOWER_LEFT");
	}
	fclose (dump);
	glFinish ();

	if (glGetError () != GL_NO_ERROR)
	{
		printf ("error GL error during the draws\n");
		return 1;
	}
	return 0;
}

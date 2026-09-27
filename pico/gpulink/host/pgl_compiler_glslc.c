/*
 * pgl_compiler_glslc.c - pgl's shader compiler on a PC (pgl_compiler.h): runs
 * tools/glslc, which compiles with Mesa's vc4 compiler. glCompileShader checks
 * one shader (glslc --check); glLinkProgram compiles the pair into a program
 * blob (glslc --pgl), which pgl loads like a binary.
 *
 * Results are cached (glslc --cache): PGL_GLSLC_CACHE, default
 * ~/.cache/pgpu-glslc.
 */
#define _GNU_SOURCE
#include "pgl_compiler.h"
#include "pgpu_program.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef PGL_GLSLC
#error "PGL_GLSLC: the path of tools/glslc/glslc.py"
#endif

static const char *cache_dir (void)
{
	static char dir[1024];
	if (!dir[0])
	{
		const char *env = getenv ("PGL_GLSLC_CACHE");
		if (env)
		{
			snprintf (dir, sizeof dir, "%s", env);
		}
		else
		{
			snprintf (dir, sizeof dir, "%s/.cache/pgpu-glslc", getenv ("HOME") ? getenv ("HOME") : "/tmp");
		}
	}
	return dir;
}

/* run argv; *output gets stdout and stderr (malloc'ed); the exit status */
static int run (char *const argv[], char **output)
{
	int pipefd[2];
	if (pipe (pipefd) < 0)
	{
		*output = strdup ("pgl: pipe failed");
		return -1;
	}
	pid_t pid = fork ();
	if (pid == 0)
	{
		dup2 (pipefd[1], 1);
		dup2 (pipefd[1], 2);
		close (pipefd[0]);
		close (pipefd[1]);
		execvp (argv[0], argv);
		_exit (127);
	}
	close (pipefd[1]);
	size_t size = 0, cap = 4096;
	char *buf = malloc (cap);
	ssize_t n;
	while ((n = read (pipefd[0], buf + size, cap - size - 1)) > 0)
	{
		size += n;
		if (cap - size < 1024)
		{
			buf = realloc (buf, cap *= 2);
		}
	}
	buf[size] = 0;
	close (pipefd[0]);
	int status;
	waitpid (pid, &status, 0);
	*output = buf;
	return WIFEXITED (status) ? WEXITSTATUS (status) : -1;
}

/* a temporary file with the source */
static char *temp_source (const char *source, const char *suffix)
{
	char *path;
	if (asprintf (&path, "/tmp/pgl-XXXXXX%s", suffix) < 0)
	{
		return NULL;
	}
	int fd = mkstemps (path, strlen (suffix));
	if (fd < 0)
	{
		free (path);
		return NULL;
	}
	size_t n = strlen (source);
	if (write (fd, source, n) != (ssize_t) n)
	{
		close (fd);
		unlink (path);
		free (path);
		return NULL;
	}
	close (fd);
	return path;
}

bool pglc_available (void)
{
	return true;
}

bool pglc_compile (unsigned type, const char *source, char **log)
{
	*log = NULL;
	char *path = temp_source (source, type == 0x8B31 ? ".vert" : ".frag");	/* GL_VERTEX_SHADER */
	if (!path)
	{
		*log = strdup ("pgl: cannot write a temporary file");
		return false;
	}
	char *argv[] = {"python3", PGL_GLSLC, "--check", type == 0x8B31 ? "--vs" : "--fs", path,
			"--cache", (char *) cache_dir (), NULL};
	char *out;
	int status = run (argv, &out);
	unlink (path);
	free (path);
	if (out[0])
	{
		*log = out;
	}
	else
	{
		free (out);
	}
	return status == 0;
}

typedef struct
{
	pgpu_program_info_t info;		/* first: the pointer pgl gets */
	uint32_t *blob;
	pgpu_attrib_info_t *attribs;
	pgpu_uniform_info_t *uniforms;
} program_t;

void pglc_free (pgpu_program_info_t *program)
{
	program_t *p = (program_t *) program;
	if (!p)
	{
		return;
	}
	for (uint32_t i = 0; i < p->info.n_attribs; i++)
	{
		free ((char *) p->attribs[i].name);
	}
	for (uint32_t i = 0; i < p->info.n_uniforms; i++)
	{
		free ((char *) p->uniforms[i].name);
		free ((uint16_t *) p->uniforms[i].offsets);
	}
	free (p->blob);
	free (p->attribs);
	free (p->uniforms);
	free (p);
}

/* glslc --pgl: "program N", "blob <hex>...", "attrib NAME LOC TYPE SIZE",
   "uniform NAME TYPE SIZE COMPONENTS SAMPLER ARRAY OFFSETS...", "end" */
static program_t *parse_program (FILE *f)
{
	program_t *p = calloc (1, sizeof *p);
	p->attribs = calloc (1, sizeof *p->attribs);
	p->uniforms = calloc (1, sizeof *p->uniforms);
	uint32_t words = 0, max_uniforms = 1, max_attribs = 1;
	bool end = false;
	char *line = NULL;
	size_t cap = 0;
	while (getline (&line, &cap, f) > 0 && !end)
	{
		char *save, *tok = strtok_r (line, " \n", &save);
		if (!tok)
		{
			continue;
		}
		if (!strcmp (tok, "program"))
		{
			p->info.words = strtoul (strtok_r (NULL, " \n", &save), NULL, 10);
			p->blob = calloc (p->info.words, 4);
		}
		else if (!strcmp (tok, "blob"))
		{
			while ((tok = strtok_r (NULL, " \n", &save)) && words < p->info.words)
			{
				p->blob[words++] = strtoul (tok, NULL, 16);
			}
		}
		else if (!strcmp (tok, "attrib"))
		{
			/* more names than locations with aliasing (GL ES 2.0 2.10.4) */
			if (p->info.n_attribs == max_attribs)
			{
				p->attribs = realloc (p->attribs, (max_attribs *= 2) * sizeof *p->attribs);
			}
			pgpu_attrib_info_t *a = &p->attribs[p->info.n_attribs++];
			a->name = strdup (strtok_r (NULL, " \n", &save));
			a->location = strtoul (strtok_r (NULL, " \n", &save), NULL, 10);
			a->type = strtoul (strtok_r (NULL, " \n", &save), NULL, 16);
			a->size = strtol (strtok_r (NULL, " \n", &save), NULL, 10);
		}
		else if (!strcmp (tok, "uniform"))
		{
			if (p->info.n_uniforms == max_uniforms)
			{
				p->uniforms = realloc (p->uniforms, (max_uniforms *= 2) * sizeof *p->uniforms);
			}
			pgpu_uniform_info_t *u = &p->uniforms[p->info.n_uniforms++];
			memset (u, 0, sizeof *u);
			u->name = strdup (strtok_r (NULL, " \n", &save));
			u->type = strtoul (strtok_r (NULL, " \n", &save), NULL, 16);
			u->size = strtol (strtok_r (NULL, " \n", &save), NULL, 10);
			u->components = strtoul (strtok_r (NULL, " \n", &save), NULL, 10);
			u->sampler = strtol (strtok_r (NULL, " \n", &save), NULL, 10);
			u->array = strtoul (strtok_r (NULL, " \n", &save), NULL, 10) != 0;
			if (u->components)
			{
				uint32_t n = 2 * u->size * u->components;
				uint16_t *offsets = calloc (n, sizeof *offsets);
				for (uint32_t i = 0; i < n && (tok = strtok_r (NULL, " \n", &save)); i++)
				{
					offsets[i] = strtoul (tok, NULL, 10);
				}
				u->offsets = offsets;
			}
		}
		else if (!strcmp (tok, "end"))
		{
			end = true;
		}
	}
	free (line);
	if (!end || !p->info.words || words != p->info.words)
	{
		pglc_free (&p->info);
		return NULL;
	}
	p->info.magic = PGPU_PROGRAM_INFO_MAGIC;
	p->info.blob = p->blob;
	p->info.attribs = p->attribs;
	p->info.uniforms = p->uniforms;
	return p;
}

pgpu_program_info_t *pglc_link (const char *vs, const char *fs, const char *const *names,
				const unsigned *locations, unsigned bindings, char **log)
{
	*log = NULL;
	char *vs_path = temp_source (vs, ".vert"), *fs_path = temp_source (fs, ".frag");
	char out_path[] = "/tmp/pgl-XXXXXX.pgl";
	int fd = mkstemps (out_path, 4);
	if (!vs_path || !fs_path || fd < 0)
	{
		*log = strdup ("pgl: cannot write a temporary file");
		return NULL;
	}
	close (fd);

	char **argv = calloc (16 + 2 * bindings, sizeof *argv);
	char **binds = calloc (bindings + 1, sizeof *binds);
	unsigned n = 0;
	argv[n++] = "python3";
	argv[n++] = PGL_GLSLC;
	argv[n++] = "--vs";
	argv[n++] = vs_path;
	argv[n++] = "--fs";
	argv[n++] = fs_path;
	argv[n++] = "--pgl";
	argv[n++] = out_path;
	argv[n++] = "--cache";
	argv[n++] = (char *) cache_dir ();
	for (unsigned i = 0; i < bindings; i++)
	{
		if (asprintf (&binds[i], "%s=%u", names[i], locations[i]) < 0)
		{
			binds[i] = NULL;
			continue;
		}
		argv[n++] = "--bind";
		argv[n++] = binds[i];
	}
	char *out;
	int status = run (argv, &out);

	program_t *p = NULL;
	if (status == 0)
	{
		FILE *f = fopen (out_path, "r");
		if (f)
		{
			p = parse_program (f);
			fclose (f);
		}
	}
	if (!p)
	{
		*log = out[0] ? out : strdup ("pgl: glslc failed");
		if (*log != out)
		{
			free (out);
		}
	}
	else
	{
		free (out);
	}

	for (unsigned i = 0; i < bindings; i++)
	{
		free (binds[i]);
	}
	free (binds);
	free (argv);
	unlink (vs_path);
	unlink (fs_path);
	unlink (out_path);
	free (vs_path);
	free (fs_path);
	return p ? &p->info : NULL;
}

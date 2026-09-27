/*
 * videoplay.c - an MP4's H.264 track played by the Zero, through its USB: the
 * samples go to the Zero's decoder as they are in the file (as much as its
 * buffer takes), the video is a texture (pglVideoTexture) drawn on a quad over
 * the screen, letterboxed. Loops: the next round's times follow on.
 *
 * The file is read as a filesystem on an SD card reads it: whole 512-byte
 * sectors only, runs of them straight into the destination, a part of a
 * sector through a one-sector cache (FatFs' way). The demuxer and the packet
 * composer get the data through that reader (pgpu_read_t): no file in memory,
 * no buffer for a sample.
 *
 *   videoplay FILE.mp4 [SECONDS]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "pgpu.h"
#include "pgpu_mp4.h"
#include "pgl.h"

#define STREAM		1

static const char *vertex_source =
	"attribute vec2 a_pos;\n"
	"uniform vec2 u_scale;\n"
	"varying vec2 v_uv;\n"
	"void main ()\n"
	"{\n"
	"	v_uv = vec2 (a_pos.x * 0.5 + 0.5, 0.5 - a_pos.y * 0.5);	// row 0 is the top\n"
	"	gl_Position = vec4 (a_pos * u_scale, 0.0, 1.0);\n"
	"}\n";

static const char *fragment_source =
	"precision mediump float;\n"
	"uniform sampler2D u_video;\n"
	"varying vec2 v_uv;\n"
	"void main ()\n"
	"{\n"
	"	gl_FragColor = texture2D (u_video, v_uv);\n"
	"}\n";

static GLuint shader (GLenum type, const char *source)
{
	GLuint s = glCreateShader (type);
	glShaderSource (s, 1, &source, NULL);
	glCompileShader (s);
	GLint ok = 0;
	glGetShaderiv (s, GL_COMPILE_STATUS, &ok);
	if (!ok)
	{
		char log[1024];
		glGetShaderInfoLog (s, sizeof log, NULL, log);
		fprintf (stderr, "videoplay: shader: %s\n", log);
		exit (1);
	}
	return s;
}

#define SECTOR		512

typedef struct
{
	FILE *f;
	uint64_t size;
	uint8_t cache[SECTOR];
	uint64_t cached;			/* the sector in cache (~0: none) */
	unsigned long sectors;			/* read from the "card" */
} sd_file_t;

static bool read_sectors (sd_file_t *sd, uint64_t sector, void *buffer, unsigned count)
{
	uint8_t *p = buffer;
	memset (p, 0, (size_t) count * SECTOR);			/* past the end: zeros */
	if (fseek (sd->f, (long) (sector * SECTOR), SEEK_SET) != 0)
	{
		return false;
	}
	size_t n = fread (p, 1, (size_t) count * SECTOR, sd->f);
	sd->sectors += count;
	return n > 0 || sector * SECTOR >= sd->size;
}

static bool sd_read (void *ctx, uint64_t offset, void *buffer, uint32_t bytes)
{
	sd_file_t *sd = ctx;
	uint8_t *p = buffer;
	while (bytes)
	{
		uint64_t sector = offset / SECTOR;
		uint32_t in = (uint32_t) (offset % SECTOR);
		if (in == 0 && bytes >= SECTOR)			/* whole sectors: straight in */
		{
			unsigned count = bytes / SECTOR;
			if (!read_sectors (sd, sector, p, count))
			{
				return false;
			}
			p += count * SECTOR;
			offset += count * SECTOR;
			bytes -= count * SECTOR;
			continue;
		}
		if (sd->cached != sector)			/* a part: through the cache */
		{
			if (!read_sectors (sd, sector, sd->cache, 1))
			{
				return false;
			}
			sd->cached = sector;
		}
		uint32_t n = SECTOR - in < bytes ? SECTOR - in : bytes;
		memcpy (p, sd->cache + in, n);
		p += n;
		offset += n;
		bytes -= n;
	}
	return true;
}

int main (int argc, char **argv)
{
	if (argc < 2)
	{
		fprintf (stderr, "usage: videoplay FILE.mp4 [SECONDS]\n");
		return 1;
	}
	unsigned seconds = argc > 2 ? (unsigned) atoi (argv[2]) : 20;

	static sd_file_t sd;
	sd.f = fopen (argv[1], "rb");
	if (!sd.f)
	{
		perror (argv[1]);
		return 1;
	}
	fseek (sd.f, 0, SEEK_END);
	sd.size = (uint64_t) ftell (sd.f);
	sd.cached = ~0ull;
	pgpu_mp4_t mp4;
	if (!pgpu_mp4_open (&mp4, sd_read, &sd, sd.size))
	{
		fprintf (stderr, "videoplay: %s: no H.264 track this reads\n", argv[1]);
		return 1;
	}
	printf ("videoplay: %s: %ux%u H.264, %u samples, %.2f s; %lu sectors read to open it\n",
		argv[1], mp4.width, mp4.height, mp4.samples, mp4.duration_us / 1e6, sd.sectors);

	pgpu_init ();
	if (!pglInit ())
	{
		fprintf (stderr, "videoplay: no Zero\n");
		return 1;
	}
	unsigned screen_w, screen_h;
	pglGetScreenSize (&screen_w, &screen_h);

	/* the texture: a power-of-two width (the video texture's rule), the
	   video's shape (height a multiple of 16); the quad letterboxed */
	unsigned tex_w = 32;
	while (tex_w < screen_w)
	{
		tex_w *= 2;
	}
	unsigned tex_h = (tex_w * mp4.height / mp4.width + 8) / 16 * 16;
	float scale_y = (float) screen_w * mp4.height / mp4.width / screen_h;
	float scale_x = 1.0f;
	if (scale_y > 1.0f)
	{
		scale_x = 1.0f / scale_y;
		scale_y = 1.0f;
	}
	GLuint texture;
	glGenTextures (1, &texture);
	if (!pglVideoTexture (texture, STREAM, tex_w, tex_h, mp4.width, mp4.height, mp4.avcc, mp4.avcc_size))
	{
		fprintf (stderr, "videoplay: no video texture\n");
		return 1;
	}
	printf ("videoplay: screen %ux%u, video texture %ux%u\n", screen_w, screen_h, tex_w, tex_h);

	GLuint program = glCreateProgram ();
	glAttachShader (program, shader (GL_VERTEX_SHADER, vertex_source));
	glAttachShader (program, shader (GL_FRAGMENT_SHADER, fragment_source));
	glBindAttribLocation (program, 0, "a_pos");
	glLinkProgram (program);
	glUseProgram (program);
	glUniform1i (glGetUniformLocation (program, "u_video"), 0);
	glUniform2f (glGetUniformLocation (program, "u_scale"), scale_x, scale_y);
	static const float quad[12] = {-1, -1, 1, -1, 1, 1, -1, -1, 1, 1, -1, 1};
	GLuint buffer;
	glGenBuffers (1, &buffer);
	glBindBuffer (GL_ARRAY_BUFFER, buffer);
	glBufferData (GL_ARRAY_BUFFER, sizeof quad, quad, GL_STATIC_DRAW);
	glEnableVertexAttribArray (0);
	glVertexAttribPointer (0, 2, GL_FLOAT, GL_FALSE, 0, (void *) 0);
	glBindTexture (GL_TEXTURE_2D, texture);
	glClearColor (0, 0, 0, 1);

	int64_t pts_base = 0;			/* the round's offset (looping) */
	pgpu_mp4_sample_t sample;
	bool have = pgpu_mp4_next (&mp4, &sample);
	uint64_t start = pgpu_time_us (), last_report = start;
	unsigned frames = 0, sent = 0;
	while (pgpu_time_us () - start < seconds * 1000000ull)
	{
		/* samples, as many as the Zero's buffer takes, from the "card"
		   straight into the packets */
		while (have && pgpu_video_room (STREAM) >= sample.size)
		{
			if (!pgpu_video_sample_read (STREAM, sample.keyframe ? PGPU_VIDEO_KEYFRAME : 0,
						     pts_base + sample.pts_us, sample.size, sd_read, &sd,
						     sample.offset))
			{
				fprintf (stderr, "videoplay: can't read sample %u\n", mp4.next - 1);
			}
			sent++;
			have = pgpu_mp4_next (&mp4, &sample);
			if (!have && !mp4.error)		/* again, the times going on */
			{
				pts_base += mp4.duration_us;
				pgpu_mp4_rewind (&mp4);
				have = pgpu_mp4_next (&mp4, &sample);
			}
		}
		if (mp4.error)
		{
			fprintf (stderr, "videoplay: the file's tables are broken or unreadable\n");
			return 1;
		}

		glClear (GL_COLOR_BUFFER_BIT);
		glDrawArrays (GL_TRIANGLES, 0, 6);
		pglSwapBuffers ();
		pgpu_flush ();
		frames++;

		/* 60 frames a second (over USB there are no FRAME pulses to pace on;
		   unpaced, frames would pile up in the Zero's buffer ahead of the
		   video's samples) */
		static uint64_t next_frame;
		next_frame = next_frame ? next_frame + 16667 : pgpu_time_us ();
		uint64_t t = pgpu_time_us ();
		if (next_frame > t)
		{
			usleep ((useconds_t) (next_frame - t));
		}
		else if (t - next_frame > 100000)
		{
			next_frame = t;
		}

		uint64_t now = pgpu_time_us ();
		if (now - last_report >= 1000000)
		{
			pgpu_video_status_t st;
			pgpu_video_get_status (STREAM, &st);
			GLenum e = glGetError ();
			printf ("videoplay: %u GL frames, %u samples sent, decoded %u shown %u dropped %u, "
				"waiting %u, at %.3f s, room %u KB, flags %x, GL error 0x%x, %lu sectors\n",
				frames, sent, st.decoded, st.shown, st.dropped, st.waiting,
				st.shown_pts == PGPU_VIDEO_TIME_NONE ? -1.0 : st.shown_pts / 1e6,
				pgpu_video_room (STREAM) / 1024, st.flags, e, sd.sectors);
			sd.sectors = 0;
			frames = sent = 0;
			last_report = now;
		}
	}

	pgpu_video_control (STREAM, PGPU_VIDEO_CLOSE, 0);
	pgpu_flush ();
	return 0;
}

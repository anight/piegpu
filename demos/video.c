/*
 * video.c - an MP4 played by the GPU: its H.264 track (pgpu_mp4) goes to the
 * RPi's decoder sample by sample as it is in the file (pgpu_video_sample_read:
 * from the file straight into the link's packets; a file on an SD card would
 * be read the same way, with its own pgpu_read_t), the video is a texture
 * (pglVideoTexture) drawn on a quad over the screen, letterboxed, with the HUD
 * over it. The MP4 is a file (PGPU_VIDEO_PATH, a host with a filesystem: the
 * ESP32-P4's microSD card) read as it's needed, or else linked in (demos.cmake:
 * PGPU_VIDEO_MP4). It loops: the
 * next round's times follow on, so the RPi's clock just runs.
 *
 * Its AAC track, if it has one, goes the same way to the RPi's audio stream
 * (pgpu_audio_open: decoded there, played on HDMI; the video follows the
 * sound's clock). Both tracks loop with the same period, the longer track's,
 * so they stay together round after round.
 *
 * The samples go as far ahead as the RPi's buffers take (pgpu_video_room),
 * at most AHEAD_US ahead of the frame on screen.
 */
#include <stdio.h>
#include <ctype.h>
#include <string.h>
#include "pico/stdlib.h"
#include "gles/pgl.h"
#include "pgpu.h"
#include "pgpu_mp4.h"
#include "hud.h"
#include "pgpu_perf.h"
#include "screen.h"
#include "video_program.h"

#define STREAM		1
#ifndef AHEAD_US
#define AHEAD_US	1500000
#endif

extern const uint8_t video_mp4[], video_mp4_end[];

#ifdef PGPU_VIDEO_PATH
#include <fcntl.h>
#include <unistd.h>

/* the file through POSIX calls (on the P4: its FatFs, reading straight into
   the packet; measured 12.8 MB/s so, where unbuffered stdio managed 84 KB/s
   and buffered 2.1 MB/s). Files up to 2 GB (lseek's off_t) */
#define SLOW_READ_US	20000
static unsigned read_calls, read_us;
static unsigned read_direct;			/* whole sectors into aligned memory (DMA) */
static uint64_t read_bytes;

static bool file_read (void *ctx, uint64_t offset, void *buffer, uint32_t bytes)
{
	int fd = (int) (intptr_t) ctx;
	uint32_t bytes0 = bytes;
	read_direct += offset % 512 == 0 && bytes >= 512 && (uintptr_t) buffer % 64 == 0;
	uint64_t t = time_us_64 ();
	bool ok = lseek (fd, (off_t) offset, SEEK_SET) == (off_t) offset;
	for (uint8_t *p = buffer; ok && bytes; )
	{
		ssize_t n = read (fd, p, bytes);
		ok = n > 0;
		p += ok ? n : 0;
		bytes -= ok ? (uint32_t) n : bytes;
		read_bytes += ok ? (uint64_t) n : 0;
	}
	unsigned us = (unsigned) (time_us_64 () - t);
	read_us += us;
	read_calls++;
	if (us >= SLOW_READ_US)				/* the card's (or FAT's) hiccups */
	{
		printf ("video: slow read: %u bytes at %llu (sector %llu + %u), %u ms\n", (unsigned) bytes0,
			(unsigned long long) offset, (unsigned long long) (offset / 512),
			(unsigned) (offset % 512), us / 1000);
	}
	return ok;
}
#endif


int main (void)
{
	stdio_init_all ();
	pgpu_init ();
	printf ("\nvideo: waiting for the RPi (READY)...\n");
	while (!pgpu_wait_ready (1000))
	{
	}
	pgpu_set_reply_phase (1);
	int tries = 0;
	while (!pglInit () && ++tries < 5)		/* the first reply can be missed */
	{
	}

	pgpu_mp4_t mp4;
	bool opened = false;
	const char *source = "the MP4 linked in";
#ifdef PGPU_VIDEO_PATH
	int fd = open (PGPU_VIDEO_PATH, O_RDONLY);
	if (fd >= 0)
	{
		off_t size = lseek (fd, 0, SEEK_END);
		opened = size > 0 && pgpu_mp4_open (&mp4, file_read, (void *) (intptr_t) fd, (uint64_t) size);
		if (opened)
		{
			source = PGPU_VIDEO_PATH;
		}
		else
		{
			printf ("video: %s isn't an MP4 with an H.264 track this reads\n", PGPU_VIDEO_PATH);
		}
	}
	else
	{
		printf ("video: no %s\n", PGPU_VIDEO_PATH);
	}
#endif
	if (!opened && !pgpu_mp4_open_memory (&mp4, video_mp4, (size_t) (video_mp4_end - video_mp4)))
	{
		printf ("video: the linked-in file isn't an MP4 with an H.264 track\n");
		return 1;
	}
	printf ("video: %s: %ux%u H.264, %u samples, %.2f s, %u MB%s%s\n", source, (unsigned) mp4.width,
		(unsigned) mp4.height, (unsigned) mp4.samples, mp4.duration_us / 1e6,
		(unsigned) (mp4.size >> 20), mp4.title[0] ? ", title " : "", mp4.title);

	/* its AAC track, from the same file */
	pgpu_mp4_t amp4;
	bool audio = mp4.memory ? pgpu_mp4_open_memory_audio (&amp4, mp4.memory, (size_t) mp4.size)
				: pgpu_mp4_open_audio (&amp4, mp4.read, mp4.ctx, mp4.size);
	if (audio)
	{
		printf ("video: AAC track: %u Hz, %u channels, %u samples, %.2f s\n",
			(unsigned) amp4.sample_rate, (unsigned) amp4.channels, (unsigned) amp4.samples,
			amp4.duration_us / 1e6);
	}
	else
	{
		printf ("video: no AAC track: no sound\n");
	}
	/* the loop's period: the longer track's */
	int64_t period = audio && amp4.duration_us > mp4.duration_us ? amp4.duration_us : mp4.duration_us;

	/* the bottom line: the file's title, in the HUD's capitals */
	char label[sizeof mp4.title] = "VIDEO";
	if (mp4.title[0])
	{
		for (size_t i = 0; i < sizeof label; i++)
		{
			label[i] = (char) toupper ((unsigned char) mp4.title[i]);
		}
	}

	GLuint prog = glCreateProgram ();
	glProgramBinaryOES (prog, PGL_PROGRAM_BINARY_PGPU, &video_info, sizeof video_info);
	GLint u_scale = glGetUniformLocation (prog, "u_scale");
	GLint a_pos = glGetAttribLocation (prog, "a_pos");
	glUseProgram (prog);
	glUniform1i (glGetUniformLocation (prog, "u_video"), 0);
	static const float quad[12] = {-1, -1, 1, -1, 1, 1, -1, -1, 1, 1, -1, 1};
	GLuint buffer;
	glGenBuffers (1, &buffer);
	glBindBuffer (GL_ARRAY_BUFFER, buffer);
	glBufferData (GL_ARRAY_BUFFER, sizeof quad, quad, GL_STATIC_DRAW);
	GLuint texture;
	glGenTextures (1, &texture);
	glClearColor (0.0f, 0.0f, 0.0f, 1.0f);
	glDisable (GL_DITHER);
	if (!hud_init ())
	{
		printf ("video: the HUD program didn't link\n");
	}

	GLint vp[4] = {0};
	unsigned tex_w = 0, tex_h = 0;
	int64_t pts_base = 0;			/* the round's offset (looping) */
	bool streaming = false;
	pgpu_mp4_sample_t sample, asample;
	bool have = pgpu_mp4_next (&mp4, &sample);
	bool ahave = audio && pgpu_mp4_next (&amp4, &asample);
	int64_t apts_base = 0;
	unsigned asent = 0;
	unsigned windows = 0, sent = 0, demux_us = 0;
	perf_t m;
	memset (&m, 0, sizeof m);
	while (true)
	{
		/* the texture: a power-of-two width (the video texture's rule), the
		   video's shape (height a multiple of 16); the quad letterboxed on the
		   screen. Again when the screen changes: the texture's resized, the
		   video goes on */
		if (screen_update ("video", vp))
		{
			/* a power of two, the screen's width or more, but not above the
			   video's: a larger texture shows no more detail and costs the
			   VideoCore's memory (at 1920x1080, a 2048-wide one left the
			   decoder out of resources with gpu_mem=128) and the GPU's time */
			unsigned want = (unsigned) vp[2] < mp4.width ? (unsigned) vp[2] : mp4.width;
			tex_w = 32;
			while (tex_w < want)
			{
				tex_w *= 2;
			}
			tex_h = (tex_w * mp4.height / mp4.width + 8) / 16 * 16;
			float sy = (float) vp[2] * mp4.height / mp4.width / vp[3], sx = 1.0f;
			if (sy > 1.0f)
			{
				sx = 1.0f / sy;
				sy = 1.0f;
			}
			glUseProgram (prog);
			glUniform2f (u_scale, sx, sy);
			if (!streaming)		/* the first time: the stream starts */
			{
				pglVideoTexture (texture, STREAM, tex_w, tex_h, mp4.width, mp4.height,
						 mp4.avcc, mp4.avcc_size);
				if (audio)		/* the sound, the video's clock */
				{
					pgpu_audio_open (STREAM, amp4.asc, amp4.asc_size);
				}
				streaming = true;
			}
			else			/* the screen changed: the video goes on */
			{
				pglVideoResize (texture, STREAM, tex_w, tex_h);
			}
			printf ("video: texture %ux%u\n", tex_w, tex_h);
		}

		/* samples: as many as the RPi takes, up to AHEAD_US ahead */
		pgpu_video_status_t st;
		pgpu_video_get_status (STREAM, &st);
		int64_t shown = st.shown_pts == PGPU_VIDEO_TIME_NONE ? 0 : st.shown_pts;
		while (   have && pts_base + sample.pts_us - shown < AHEAD_US
		       && pgpu_video_room (STREAM) >= sample.size)
		{
			if (!pgpu_video_sample_read (STREAM, sample.keyframe ? PGPU_VIDEO_KEYFRAME : 0,
						     pts_base + sample.pts_us, sample.size, mp4.read, mp4.ctx,
						     sample.offset))
			{
				printf ("video: can't read sample %u\n", (unsigned) mp4.next - 1);
			}
			sent++;
			uint64_t t = time_us_64 ();
			have = pgpu_mp4_next (&mp4, &sample);
			demux_us += (unsigned) (time_us_64 () - t);
			if (!have && !mp4.error)	/* again, the times going on */
			{
				pts_base += period;
				pgpu_mp4_rewind (&mp4);
				have = pgpu_mp4_next (&mp4, &sample);
			}
		}
		while (   ahave && apts_base + asample.pts_us - shown < AHEAD_US
		       && pgpu_video_room (PGPU_AUDIO_STREAM) >= asample.size)
		{
			if (!pgpu_video_sample_read (PGPU_AUDIO_STREAM, 0, apts_base + asample.pts_us, asample.size,
						     amp4.read, amp4.ctx, asample.offset))
			{
				printf ("video: can't read AAC sample %u\n", (unsigned) amp4.next - 1);
			}
			asent++;
			ahave = pgpu_mp4_next (&amp4, &asample);
			if (!ahave && !amp4.error)
			{
				apts_base += period;
				pgpu_mp4_rewind (&amp4);
				ahave = pgpu_mp4_next (&amp4, &asample);
			}
		}

		glClear (GL_COLOR_BUFFER_BIT);
		glUseProgram (prog);
		glActiveTexture (GL_TEXTURE0);
		glBindTexture (GL_TEXTURE_2D, texture);
		glBindBuffer (GL_ARRAY_BUFFER, buffer);
		glEnableVertexAttribArray (a_pos);
		glVertexAttribPointer (a_pos, 2, GL_FLOAT, GL_FALSE, 0, (void *) 0);
		glDrawArrays (GL_TRIANGLES, 0, 6);
		hud_draw ();
		pglSwapBuffers ();

		absolute_time_t wait_start = get_absolute_time ();
		pgpu_wait_frame (100);			/* pace on the screen (swap interval 1) */
		if (perf_frame (absolute_time_diff_us (wait_start, get_absolute_time ()), &m))
		{
			hud_begin ();
			hud_perf (vp[2] - hud_perf_width (0.5f) - 2, 2, 0.5f, &m);
			hud_text_scaled (4, vp[3] - 12, label, HUD_RGBA (255, 255, 255, 200), 0.5f);
			hud_end ();
			if (++windows % 5 == 0)
			{
				GLenum e = glGetError ();
				printf ("video: %.1f fps, GPU %.0f%% CPU-G %.0f%% CPU-H %.0f%%; decoded %u shown %u "
					"dropped %u, waiting %u, at %.2f s, %u samples sent, room %u KB, "
					"flags %x, GL error 0x%x\n", m.fps, m.gpu * 100, m.cpu_g * 100,
					m.cpu_h * 100, (unsigned) st.decoded, (unsigned) st.shown,
					(unsigned) st.dropped, (unsigned) st.waiting, shown / 1e6, sent,
					(unsigned) (pgpu_video_room (STREAM) / 1024), (unsigned) st.flags,
					(unsigned) e);
				sent = 0;
				if (audio)
				{
					pgpu_video_status_t as;
					pgpu_video_get_status (PGPU_AUDIO_STREAM, &as);
					printf ("video: sound: %u units decoded, %u broken, %u ms queued, %u ms of "
						"silence, heard %.2f s, %u samples sent, flags %x\n",
						(unsigned) as.decoded, (unsigned) as.dropped, (unsigned) as.waiting,
						(unsigned) as.shown, as.shown_pts == PGPU_VIDEO_TIME_NONE ? -1.0
						: as.shown_pts / 1e6, asent, (unsigned) as.flags);
					asent = 0;
				}
#ifdef PGPU_VIDEO_PATH
				printf ("video: file: %u reads (%u of whole sectors into aligned memory), %u KB in "
					"%u ms (%u KB/s while reading); demux %u ms\n", read_calls, read_direct,
					(unsigned) (read_bytes / 1024), read_us / 1000,
					read_us ? (unsigned) (read_bytes * 1000 / read_us) : 0, demux_us / 1000);
				read_calls = read_us = read_direct = 0;
				read_bytes = 0;
#endif
				demux_us = 0;
			}
		}
	}
}

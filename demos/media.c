/*
 * media.c - an MP4, an MP3 or an Ogg Vorbis file played by the GPU.
 *
 * An MP4's H.264 track (pgpu_mp4) goes to the RPi's decoder sample by sample
 * as it is in the file (pgpu_media_sample_read: from the file straight into
 * the link's packets; a file on an SD card would be read the same way, with
 * its own pgpu_read_t), the video is a texture (pglVideoTexture) drawn on a
 * quad over the screen, letterboxed, with the HUD over it. Its AAC track, if
 * it has one, goes the same way to the RPi's audio stream (pgpu_audio_open:
 * decoded there, played on HDMI; the video follows the sound's clock). Both
 * tracks loop with the same period, the longer track's, so they stay
 * together round after round.
 *
 * An MP3 (pgpu_mp3: MPEG audio frames) goes to the audio stream frame by
 * frame the same way, an Ogg Vorbis file (pgpu_ogg) page by page. Sound alone
 * (an MP3, an Ogg, or an MP4 without video) shows what's playing: title,
 * artist, the stream, the time on a bar.
 *
 * The file: PGPU_MEDIA_PATH (a host with a filesystem: the ESP32-P4's microSD
 * card, the page's file for hosts/web), the PGPU_MEDIA environment variable
 * (hosts/pc: PGPU_MEDIA_ENV), read as it's needed (on the P4 read ahead by
 * the host's reader, PGPU_MEDIA_READER: hosts/esp32p4/main/media_reader.h);
 * or else the one linked in (demos.cmake: PGPU_MEDIA_EMBED). It loops: the
 * next round's times follow on, so the RPi's clock just runs.
 *
 * The volume: '+' and '-' on the console (10% a step), 'm' mutes and unmutes;
 * on the installer page, its slider. Jumps, on the console: 'f' and 'b' 30 s
 * forward and back, 'F' and 'B' 5 minutes (from the last keyframe before the
 * place: both streams are opened again there). The samples go as far ahead as the RPi's
 * buffers take (pgpu_media_room), at most AHEAD_US ahead of what's shown or
 * heard.
 */
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>
#include "pico/stdlib.h"
#include "gles/pgl.h"
#include "pgpu.h"
#include "pgpu_mp4.h"
#include "pgpu_mp3.h"
#include "pgpu_ogg.h"
#include "hud.h"
#include "pgpu_perf.h"
#include "screen.h"
#include "video_program.h"
#ifdef __EMSCRIPTEN__
#include <emscripten.h>

/* the page's volume slider (web/installer: GLStream.volumeRequest): a new
   volume in percent, or -1 */
EM_JS (int, web_volume_request, (void), {
	const v = Module.glIO.volumeRequest;
	Module.glIO.volumeRequest = -1;
	return v === undefined ? -1 : v;
});
#endif
#define STREAM		1
#ifndef AHEAD_US
#define AHEAD_US	1500000
#endif

extern const uint8_t media_file[], media_file_end[];

#if defined(PGPU_MEDIA_PATH) || defined(PGPU_MEDIA_ENV)
#define MEDIA_FILES	1
#include <fcntl.h>
#include <unistd.h>
#ifdef PGPU_MEDIA_READER
#include "media_reader.h"
#endif

/* the file to play: the environment's, else PGPU_MEDIA_PATH; NULL: none */
static const char *media_path (void)
{
	const char *path = NULL;
#ifdef PGPU_MEDIA_ENV
	path = getenv ("PGPU_MEDIA");
#endif
#ifdef PGPU_MEDIA_PATH
	path = path ? path : PGPU_MEDIA_PATH;
#endif
	return path;
}

/* the file through POSIX calls (on the P4: its FatFs, reading straight into
   the packet; measured 12.8 MB/s so, where unbuffered stdio managed 84 KB/s
   and buffered 2.1 MB/s). Files up to 2 GB (lseek's off_t) */
#define SLOW_READ_US	20000
static unsigned read_calls, read_us;
static unsigned read_direct;			/* whole sectors into aligned memory (DMA) */
static uint64_t read_bytes;

#ifndef PGPU_MEDIA_READER
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
		printf ("media: slow read: %u bytes at %llu (sector %llu + %u), %u ms\n", (unsigned) bytes0,
			(unsigned long long) offset, (unsigned long long) (offset / 512),
			(unsigned) (offset % 512), us / 1000);
	}
	return ok;
}
#endif
#endif

/* the sound: an MP4's AAC track, an MP3's frames or an Ogg's pages */
typedef struct
{
	enum { SOUND_AAC, SOUND_MP3, SOUND_OGG } kind;
	pgpu_mp4_t amp4;
	pgpu_mp3_t mp3s;
	pgpu_ogg_t oggs;
} sound_t;

static bool sound_next (sound_t *s, pgpu_mp4_sample_t *sample)
{
	if (s->kind == SOUND_AAC)
	{
		return pgpu_mp4_next (&s->amp4, sample);
	}
	pgpu_mp3_sample_t f;
	pgpu_ogg_sample_t p;
	bool got = s->kind == SOUND_MP3 ? pgpu_mp3_next (&s->mp3s, &f) : pgpu_ogg_next (&s->oggs, &p);
	if (!got)
	{
		return false;
	}
	sample->offset = s->kind == SOUND_MP3 ? f.offset : p.offset;
	sample->size = s->kind == SOUND_MP3 ? f.size : p.size;
	sample->pts_us = sample->dts_us = s->kind == SOUND_MP3 ? f.pts_us : p.pts_us;
	sample->keyframe = true;
	return true;
}

static void sound_rewind (sound_t *s)
{
	switch (s->kind)
	{
	case SOUND_AAC:	pgpu_mp4_rewind (&s->amp4); break;
	case SOUND_MP3:	pgpu_mp3_rewind (&s->mp3s); break;
	case SOUND_OGG:	pgpu_ogg_rewind (&s->oggs); break;
	}
}

static bool sound_error (const sound_t *s)
{
	return s->kind == SOUND_AAC ? s->amp4.error : s->kind == SOUND_MP3 ? s->mp3s.error : s->oggs.error;
}
static int64_t sound_duration (const sound_t *s)
{
	return   s->kind == SOUND_AAC ? s->amp4.duration_us : s->kind == SOUND_MP3 ? s->mp3s.duration_us
	       : s->oggs.duration_us;
}
static pgpu_read_t sound_reader (const sound_t *s)
{
	return s->kind == SOUND_AAC ? s->amp4.read : s->kind == SOUND_MP3 ? s->mp3s.read : s->oggs.read;
}
static void *sound_ctx (const sound_t *s)
{
	return s->kind == SOUND_AAC ? s->amp4.ctx : s->kind == SOUND_MP3 ? s->mp3s.ctx : s->oggs.ctx;
}

static void sound_open (const sound_t *s, uint32_t video_stream)
{
	switch (s->kind)
	{
	case SOUND_AAC:
		pgpu_audio_open (PGPU_AUDIO_AAC, video_stream, s->amp4.asc, s->amp4.asc_size);
		break;
	case SOUND_MP3:
		pgpu_audio_open (PGPU_AUDIO_MP3, video_stream, s->mp3s.header, sizeof s->mp3s.header);
		break;
	case SOUND_OGG:
		pgpu_audio_open (PGPU_AUDIO_VORBIS, video_stream, s->oggs.config, s->oggs.config_size);
		break;
	}
}

/* the video track from its last keyframe at or before target (the file's
   time, microseconds): *sample is that keyframe, the next one after it
   follows; false if the track has none */
static bool seek_video (pgpu_mp4_t *m, int64_t target, pgpu_mp4_sample_t *sample)
{
	static pgpu_mp4_t at;			/* (the reader's state after the keyframe: big) */
	pgpu_mp4_sample_t s;
	bool found = false;
	unsigned n = 0;
	uint64_t t0 = time_us_64 ();
	pgpu_mp4_rewind (m);
	while (pgpu_mp4_next (m, &s) && s.dts_us <= target)
	{
		if (++n % 4096 == 0)			/* (a long way: say how it goes) */
		{
			printf ("media: seeking: %u samples, at %.1f s, %u ms\n", n, s.dts_us / 1e6,
				(unsigned) ((time_us_64 () - t0) / 1000));
		}
		if (s.keyframe)
		{
			at = *m;
			*sample = s;
			found = true;
		}
	}
	if (!found || m->error)
	{
		pgpu_mp4_rewind (m);
		return pgpu_mp4_next (m, sample);
	}
	*m = at;
	return true;
}

/* the sound from its first sample at or after target (an Ogg's from the page
   before it, which the RPi's decoder only primes itself on: pgpu_ogg_seek) */
static bool sound_seek (sound_t *s, int64_t target, pgpu_mp4_sample_t *sample)
{
	if (s->kind == SOUND_OGG)
	{
		pgpu_ogg_sample_t p;
		if (!pgpu_ogg_seek (&s->oggs, target, &p))
		{
			return false;
		}
		sample->offset = p.offset;
		sample->size = p.size;
		sample->pts_us = sample->dts_us = p.pts_us;
		sample->keyframe = true;
		return true;
	}
	sound_rewind (s);
	while (sound_next (s, sample))
	{
		if (sample->pts_us >= target)
		{
			return true;
		}
	}
	sound_rewind (s);
	return sound_next (s, sample);
}

/* text for the HUD's font (capitals, digits, . % : -): in capitals, other
   characters (and UTF-8's) as spaces, at most max characters */
static void hud_string (char *out, const char *in, size_t max)
{
	size_t n = 0;
	for (; *in && n < max; in++)
	{
		unsigned char c = (unsigned char) *in;
		if ((c & 0xC0) == 0x80)
		{
			continue;			/* (a UTF-8 sequence: one space) */
		}
		out[n++] = c < 0x80 ? (char) toupper (c) : ' ';
	}
	out[n] = '\0';
}

/* m:ss */
static void format_time (char *out, size_t size, int64_t us)
{
	unsigned s = us > 0 ? (unsigned) (us / 1000000) : 0;
	snprintf (out, size, "%u:%02u", s / 60, s % 60);
}

/* sound alone: what's playing, in the middle of the screen */
static void hud_now_playing (const GLint *vp, const char *title, const char *artist, const char *stream,
			     int64_t heard_us, int64_t duration_us, int volume, bool muted)
{
	int k = vp[2] / 320;				/* 1 on the panel, up to 4 on HDMI */
	float u = (float) (k < 1 ? 1 : k > 4 ? 4 : k), small = u / 2;
	float w = (float) vp[2], h = (float) vp[3];
	char line[80];

	size_t chars = w / (HUD_CHAR_W * u) > 3 ? (size_t) (w / (HUD_CHAR_W * u)) - 2 : 1;
	hud_string (line, title[0] ? title : "SOUND", chars < sizeof line - 1 ? chars : sizeof line - 1);
	float y = (float) (int) (h * 0.28f);
	hud_text_scaled ((float) (int) ((w - strlen (line) * HUD_CHAR_W * u) / 2), y, line,
			 HUD_RGBA (255, 255, 255, 255), u);
	if (artist[0])
	{
		hud_string (line, artist, chars < sizeof line - 1 ? chars : sizeof line - 1);
		y += 22 * u;
		hud_text_scaled ((float) (int) ((w - strlen (line) * HUD_CHAR_W * u) / 2), y, line,
				 HUD_RGBA (170, 200, 255, 255), u);
	}
	y += 26 * u;
	hud_string (line, stream, sizeof line - 1);
	hud_text_scaled ((float) (int) ((w - strlen (line) * HUD_CHAR_W * small) / 2), y, line,
			 HUD_RGBA (160, 160, 160, 255), small);

	/* the time on a bar: this round's */
	int64_t at = heard_us < 0 ? 0 : duration_us > 0 ? heard_us % duration_us : heard_us;
	float x0 = (float) (int) (w * 0.1f), bw = (float) (int) (w * 0.8f), by = (float) (int) (h * 0.66f);
	float fill = duration_us > 0 ? bw * (float) at / (float) duration_us : 0.0f;
	hud_rect (x0, by, bw, 4 * u, HUD_RGBA (80, 80, 80, 255));
	hud_rect (x0, by, fill, 4 * u, HUD_RGBA (120, 255, 120, 255));
	char t[16];
	format_time (t, sizeof t, at);
	hud_text_scaled (x0, by + 8 * u, t, HUD_RGBA (255, 255, 255, 220), small);
	format_time (t, sizeof t, duration_us);
	hud_text_scaled (x0 + bw - strlen (t) * HUD_CHAR_W * small, by + 8 * u, t, HUD_RGBA (255, 255, 255, 220),
			 small);

	if (volume >= 0)
	{
		snprintf (line, sizeof line, muted ? "VOLUME %d%% - MUTED" : "VOLUME %d%%", volume);
		hud_text_scaled ((float) (int) ((w - strlen (line) * HUD_CHAR_W * small) / 2),
				 (float) (int) (h * 0.84f), line, HUD_RGBA (160, 160, 160, 255), small);
	}
}

int main (void)
{
	stdio_init_all ();
	pgpu_init ();
	printf ("\nmedia: waiting for the RPi (READY)...\n");
	while (!pgpu_wait_ready (1000))
	{
	}
	pgpu_set_reply_phase (1);
	int tries = 0;
	while (!pglInit () && ++tries < 5)		/* the first reply can be missed */
	{
	}

	/* the file: an MP4 (its H.264 track and its AAC track, either may be
	   missing), an Ogg Vorbis file or an MP3 */
	pgpu_mp4_t mp4;
	static sound_t snd;
	bool video = false, audio = false, file = false;
	const char *source = "the file linked in";
#ifdef MEDIA_FILES
	const char *path = media_path ();
	pgpu_read_t reader = NULL;
	void *ctx = NULL;
	uint64_t size = 0;
#ifdef PGPU_MEDIA_READER
	if (!path || !media_reader_open (path, &reader, &ctx, &size))
	{
		size = 0;
	}
#else
	int fd = path ? open (path, O_RDONLY) : -1;
	off_t end = fd >= 0 ? lseek (fd, 0, SEEK_END) : 0;
	reader = file_read;
	ctx = (void *) (intptr_t) fd;
	size = end > 0 ? (uint64_t) end : 0;
#endif
	if (size > 0)
	{
		video = pgpu_mp4_open (&mp4, reader, ctx, size);
		audio = pgpu_mp4_open_audio (&snd.amp4, reader, ctx, size);
		if (!video && !audio && (audio = pgpu_ogg_open (&snd.oggs, reader, ctx, size)))
		{
			snd.kind = SOUND_OGG;
		}
		if (!video && !audio && (audio = pgpu_mp3_open (&snd.mp3s, reader, ctx, size)))
		{
			snd.kind = SOUND_MP3;
		}
		file = video || audio;
		if (file)
		{
			source = path;
		}
		else
		{
			printf ("media: %s isn't an MP4 with an H.264 or AAC track, an Ogg Vorbis file or an MP3\n", path);
		}
	}
	else if (path)
	{
		printf ("media: no %s\n", path);
	}
#endif
	if (!file)
	{
		size_t size = (size_t) (media_file_end - media_file);
		video = pgpu_mp4_open_memory (&mp4, media_file, size);
		audio = pgpu_mp4_open_memory_audio (&snd.amp4, media_file, size);
		if (!video && !audio && (audio = pgpu_ogg_open_memory (&snd.oggs, media_file, size)))
		{
			snd.kind = SOUND_OGG;
		}
		if (!video && !audio && (audio = pgpu_mp3_open_memory (&snd.mp3s, media_file, size)))
		{
			snd.kind = SOUND_MP3;
		}
	}
	if (!video && !audio)
	{
		printf ("media: the file isn't an MP4 with an H.264 or AAC track, an Ogg Vorbis file or an MP3\n");
		return 1;
	}
	if (video)
	{
		printf ("media: %s: %ux%u H.264, %u samples, %.2f s, %u MB%s%s\n", source, (unsigned) mp4.width,
			(unsigned) mp4.height, (unsigned) mp4.samples, mp4.duration_us / 1e6,
			(unsigned) (mp4.size >> 20), mp4.title[0] ? ", title " : "", mp4.title);
	}
	else
	{
		printf ("media: %s: no H.264 track: sound only\n", source);
	}
	char stream[80] = "";			/* the sound, for the screen */
	const char *title =   video ? mp4.title : snd.kind == SOUND_MP3 ? snd.mp3s.title
			    : snd.kind == SOUND_OGG ? snd.oggs.title : snd.amp4.title;
	const char *artist = snd.kind == SOUND_MP3 ? snd.mp3s.artist : snd.kind == SOUND_OGG ? snd.oggs.artist : "";
	if (snd.kind == SOUND_OGG)
	{
		const pgpu_ogg_t *o = &snd.oggs;
		printf ("media: Ogg Vorbis: %u Hz, %u channels, %u kbps, %.2f s, %u bytes of headers%s%s%s%s\n",
			(unsigned) o->sample_rate, (unsigned) o->channels, (unsigned) (o->bitrate / 1000),
			o->duration_us / 1e6, (unsigned) o->config_size, o->title[0] ? ", title " : "", o->title,
			o->artist[0] ? ", artist " : "", o->artist);
		int n = snprintf (stream, sizeof stream, "OGG VORBIS  %u.%u KHZ  %s", (unsigned) o->sample_rate / 1000,
				  (unsigned) o->sample_rate % 1000 / 100,
				  o->channels == 1 ? "MONO" : o->channels == 2 ? "STEREO" : "SURROUND");
		if (o->bitrate)
		{
			snprintf (stream + n, sizeof stream - n, "  %u KBPS", (unsigned) (o->bitrate / 1000));
		}
	}
	else if (snd.kind == SOUND_MP3)
	{
		const pgpu_mp3_t *m = &snd.mp3s;
		printf ("media: MP3 (layer %u): %u Hz, %u channels, %u kbps, %s%.2f s%s%s%s%s\n",
			(unsigned) m->layer, (unsigned) m->sample_rate, (unsigned) m->channels,
			(unsigned) m->bitrate_kbps, m->duration_exact ? "" : "about ", m->duration_us / 1e6,
			m->title[0] ? ", title " : "", m->title, m->artist[0] ? ", artist " : "", m->artist);
		snprintf (stream, sizeof stream, "MP3  %u.%u KHZ  %s  %u KBPS", (unsigned) m->sample_rate / 1000,
			  (unsigned) m->sample_rate % 1000 / 100, m->channels == 1 ? "MONO" : "STEREO",
			  (unsigned) m->bitrate_kbps);
	}
	else if (audio)
	{
		printf ("media: AAC track: %u Hz, %u channels, %u samples, %.2f s\n",
			(unsigned) snd.amp4.sample_rate, (unsigned) snd.amp4.channels, (unsigned) snd.amp4.samples,
			snd.amp4.duration_us / 1e6);
		snprintf (stream, sizeof stream, "AAC  %u.%u KHZ  %s", (unsigned) snd.amp4.sample_rate / 1000,
			  (unsigned) snd.amp4.sample_rate % 1000 / 100,
			  snd.amp4.channels == 1 ? "MONO" : snd.amp4.channels == 2 ? "STEREO" : "SURROUND");
	}
	else
	{
		printf ("media: no AAC track: no sound\n");
	}
	/* the loop's period with video: the longer track's (sound alone: its
	   own, exact once a round has been read) */
	int64_t period =   audio && sound_duration (&snd) > mp4.duration_us ? sound_duration (&snd)
			 : mp4.duration_us;

	/* the video's bottom line: the file's title, in the HUD's capitals */
	char label[sizeof mp4.title];
	strcpy (label, "VIDEO");
	if (video && title[0])
	{
		hud_string (label, title, sizeof label - 1);
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
		printf ("media: the HUD program didn't link\n");
	}

	GLint vp[4] = {0};
	unsigned tex_w = 0, tex_h = 0;
	int64_t pts_base = 0;			/* the round's offset (looping) */
	bool streaming = false;
	pgpu_mp4_sample_t sample, asample;
	bool have = video && pgpu_mp4_next (&mp4, &sample);
	bool ahave = audio && sound_next (&snd, &asample);
	int64_t apts_base = 0;
	int64_t origin = 0;			/* where the streams (re)started, as sent: the time
						   until they show something */
	unsigned asent = 0;
	int volume = -1;			/* percent (-1: not known yet, the RPi's status says) */
	bool muted = false;
	unsigned windows = 0, sent = 0, demux_us = 0;
	uint64_t hud_at = 0;
	perf_t m;
	memset (&m, 0, sizeof m);
	while (true)
	{
		/* the texture: a power-of-two width (the video texture's rule), the
		   video's shape (height a multiple of 16); the quad letterboxed on the
		   screen. Again when the screen changes: the texture's resized, the
		   video goes on */
		bool screen_changed = screen_update ("media", vp);
		if (!video && !streaming)
		{
			sound_open (&snd, 0);		/* sound only: its own clock */
			streaming = true;
		}
		if (video && screen_changed)
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
					sound_open (&snd, STREAM);
				}
				streaming = true;
			}
			else			/* the screen changed: the video goes on */
			{
				pglVideoResize (texture, STREAM, tex_w, tex_h);
			}
			printf ("media: texture %ux%u\n", tex_w, tex_h);
		}

		/* samples: as many as the RPi takes, up to AHEAD_US ahead */
		pgpu_media_status_t st;
		pgpu_media_get_status (video ? STREAM : PGPU_AUDIO_STREAM, &st);	/* (sound only: the time heard) */
		int64_t shown = st.shown_pts == PGPU_MEDIA_TIME_NONE ? origin : st.shown_pts;
		while (   have && pts_base + sample.pts_us - shown < AHEAD_US
		       && pgpu_media_room (STREAM) >= sample.size)
		{
			if (!pgpu_media_sample_read (STREAM, sample.keyframe ? PGPU_MEDIA_KEYFRAME : 0,
						     pts_base + sample.pts_us, sample.size, mp4.read, mp4.ctx,
						     sample.offset))
			{
				printf ("media: can't read sample %u\n", (unsigned) mp4.next - 1);
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
		       && pgpu_media_room (PGPU_AUDIO_STREAM) >= asample.size)
		{
			if (!pgpu_media_sample_read (PGPU_AUDIO_STREAM, 0, apts_base + asample.pts_us, asample.size,
						     sound_reader (&snd), sound_ctx (&snd), asample.offset))
			{
				printf ("media: can't read sound sample at %.3f s\n", asample.pts_us / 1e6);
			}
			asent++;
			ahave = sound_next (&snd, &asample);
			if (!ahave && !sound_error (&snd))
			{
				apts_base += video ? period : sound_duration (&snd);
				sound_rewind (&snd);
				ahave = sound_next (&snd, &asample);
			}
		}

		/* the console's keys: jumps; the volume (and the page's slider) */
		int c, jump = 0;
		bool changed = false;
		while ((c = getchar_timeout_us (0)) != PICO_ERROR_TIMEOUT)
		{
			jump += c == 'f' ? 30 : c == 'b' ? -30 : c == 'F' ? 300 : c == 'B' ? -300 : 0;
			if (audio)
			{
				if (volume >= 0 && (c == '+' || c == '=' || c == '-'))
				{
					volume += c == '-' ? -10 : 10;
					muted = false;
					changed = true;
				}
				else if (volume >= 0 && (c == 'm' || c == 'M'))
				{
					muted = !muted;
					changed = true;
				}
			}
		}
		if (jump && streaming)
		{
			/* from where it is now, in this round, to the place asked */
			int64_t now = shown - (video ? pts_base : apts_base);
			int64_t end = video ? period : sound_duration (&snd);
			if (now < 0)			/* (the next round is being sent, the last one shown) */
			{
				now += end;
			}
			int64_t target = now + (int64_t) jump * 1000000;
			target = target < 0 ? 0 : target > end - 5000000 ? (end > 5000000 ? end - 5000000 : 0) : target;
			if (video)
			{
				have = seek_video (&mp4, target, &sample);
				target = sample.pts_us;		/* (the sound from the keyframe's time) */
				pgpu_media_control (STREAM, PGPU_MEDIA_CLOSE, 0);
				pglVideoTexture (texture, STREAM, tex_w, tex_h, mp4.width, mp4.height, mp4.avcc,
						 mp4.avcc_size);
			}
			if (audio)
			{
				ahave = sound_seek (&snd, target, &asample);
				sound_open (&snd, video ? STREAM : 0);
			}
			origin = (video ? pts_base : apts_base) + target;
			char t[16];
			format_time (t, sizeof t, target);
			printf ("media: jumped to %s (%+d s asked, from %.1f s)\n", t, jump, now / 1e6);
#ifdef PGPU_MEDIA_READER
			char line[200];
			media_reader_stats (ctx, line, sizeof line);
			printf ("media: file while jumping: %s\n", line);
#endif
		}
		if (audio)
		{
			pgpu_media_status_t as;
			if (   volume < 0 && pgpu_media_get_status (PGPU_AUDIO_STREAM, &as)
			    && (as.flags & PGPU_MEDIA_OPEN_FLAG))
			{
				volume = (int) PGPU_AUDIO_STATUS_VOLUME (as.flags);	/* the RPi's volume= */
				printf ("media: volume %d%% ('+', '-', 'm' on the console)\n", volume);
			}
#ifdef __EMSCRIPTEN__
			int w = web_volume_request ();
			if (w >= 0)
			{
				volume = w;
				muted = false;
				changed = true;
			}
#endif
			if (changed)
			{
				volume = volume < 0 ? 0 : volume > 100 ? 100 : volume;
				pgpu_audio_volume (muted ? 0 : (uint32_t) volume);
				printf ("media: volume %d%%%s\n", volume, muted ? ", muted" : "");
			}
		}

		glClear (GL_COLOR_BUFFER_BIT);
		if (video)
		{
			glUseProgram (prog);
			glActiveTexture (GL_TEXTURE0);
			glBindTexture (GL_TEXTURE_2D, texture);
			glBindBuffer (GL_ARRAY_BUFFER, buffer);
			glEnableVertexAttribArray (a_pos);
			glVertexAttribPointer (a_pos, 2, GL_FLOAT, GL_FALSE, 0, (void *) 0);
			glDrawArrays (GL_TRIANGLES, 0, 6);
		}
		hud_draw ();
		pglSwapBuffers ();

		absolute_time_t wait_start = get_absolute_time ();
		pgpu_wait_frame (100);			/* pace on the screen (swap interval 1) */
		bool second = perf_frame (absolute_time_diff_us (wait_start, get_absolute_time ()), &m);
		uint64_t now = time_us_64 ();
		if (second || (!video && now - hud_at >= 100000))	/* (sound alone: its time, 10 a second) */
		{
			hud_at = now;
			hud_begin ();
			hud_perf (vp[2] - hud_perf_width (0.5f) - 2, 2, 0.5f, &m);
			if (video)
			{
				hud_text_scaled (4, vp[3] - 12, label, HUD_RGBA (255, 255, 255, 200), 0.5f);
			}
			else
			{
				hud_now_playing (vp, title, artist, stream,
						 st.shown_pts == PGPU_MEDIA_TIME_NONE ? -1 : st.shown_pts,
						 sound_duration (&snd), volume, muted);
			}
			hud_end ();
		}
		if (second && ++windows % 5 == 0)
		{
			GLenum e = glGetError ();
			if (video)
			{
				printf ("media: %.1f fps, GPU %.0f%% CPU-G %.0f%% CPU-H %.0f%%; decoded %u shown %u "
					"dropped %u, waiting %u, at %.2f s, %u samples sent, room %u KB, "
					"flags %x, GL error 0x%x\n", m.fps, m.gpu * 100, m.cpu_g * 100,
					m.cpu_h * 100, (unsigned) st.decoded, (unsigned) st.shown,
					(unsigned) st.dropped, (unsigned) st.waiting, shown / 1e6, sent,
					(unsigned) (pgpu_media_room (STREAM) / 1024), (unsigned) st.flags,
					(unsigned) e);
			}
			sent = 0;
			if (e != GL_NO_ERROR)
			{
				uint32_t r[3];
				pglGetRPiError (r);
				printf ("media: the RPi's last error: code %u, opcode 0x%02x, detail %u\n", (unsigned) r[0],
					(unsigned) r[1], (unsigned) r[2]);
			}
			perf_log_link ("media", &m);
			if (audio)
			{
				pgpu_media_status_t as;
				pgpu_media_get_status (PGPU_AUDIO_STREAM, &as);
				printf ("media: sound: %u units decoded, %u broken, %u ms queued, %u ms of "
					"silence, heard %.2f s, %u samples sent, flags %x\n",
					(unsigned) as.decoded, (unsigned) as.dropped, (unsigned) as.waiting,
					(unsigned) as.shown, as.shown_pts == PGPU_MEDIA_TIME_NONE ? -1.0
					: as.shown_pts / 1e6, asent, (unsigned) as.flags);
				asent = 0;
			}
#ifdef MEDIA_FILES
			if (file)
			{
#ifdef PGPU_MEDIA_READER
				char line[200];
				media_reader_stats (ctx, line, sizeof line);
				printf ("media: file: %s; demux %u ms\n", line, demux_us / 1000);
#else
				printf ("media: file: %u reads (%u of whole sectors into aligned memory), %u KB in "
					"%u ms (%u KB/s while reading); demux %u ms\n", read_calls, read_direct,
					(unsigned) (read_bytes / 1024), read_us / 1000,
					read_us ? (unsigned) (read_bytes * 1000 / read_us) : 0, demux_us / 1000);
#endif
			}
			read_calls = read_us = read_direct = 0;
			read_bytes = 0;
#endif
			demux_us = 0;
		}
	}
}

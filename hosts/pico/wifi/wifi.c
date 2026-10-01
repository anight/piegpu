/*
 * wifi - Wi-Fi for the Pico 2 W, asked of a phone (wifi_setup.h), shown on
 * the RPi's screen:
 *
 * - no network kept (or the stick's button held at the start, or "s" on the
 *   console: set up again): the Pico is an access point; the screen shows its
 *   QR code and "SCAN TO SETUP WIFI". A phone's camera offers to join it;
 * - the phone is on it: the phone offers the page by itself (or doesn't: the
 *   screen then shows the page's address, as a QR code too). The page asks
 *   for the network's name (the ones found around are offered) and its
 *   password;
 * - the network is joined: the screen says so, with our address, the signal
 *   and the time from the net, and it's kept for the next start. Not joined:
 *   the access point is back and the screen and the page say why.
 */
#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "gles/pgl.h"
#include "pgpu.h"
#include "hud.h"
#include "screen.h"
#include "texview_program.h"
#include "qrcodegen.h"
#include "wifi_setup.h"

#define QR_VERSION_MAX	10			/* 57 modules: 213 bytes of text and more */
#define QR_QUIET	4			/* modules of white around it */
#define QR_SIDE_MAX	(17 + 4 * QR_VERSION_MAX + 2 * QR_QUIET)
#define SETUP_PIN	22			/* the stick's button (active low): held at the start, set up again */

#define WHITE		HUD_RGBA (255, 255, 255, 255)
#define GREY		HUD_RGBA (170, 175, 185, 255)
#define YELLOW		HUD_RGBA (255, 215, 40, 255)
#define GREEN		HUD_RGBA (90, 220, 110, 255)
#define RED		HUD_RGBA (255, 110, 100, 255)

static GLuint view, quad, qr_texture;
static GLint v_rect, v_mode, v_pos;
static int qr_side;				/* modules, the quiet ones too (0: none) */
static char qr_text[96];

/* the text as a texture: a texel a module, white around it */
static void qr_make (const char *text)
{
	static uint8_t code[qrcodegen_BUFFER_LEN_FOR_VERSION (QR_VERSION_MAX)], temp[qrcodegen_BUFFER_LEN_FOR_VERSION (QR_VERSION_MAX)];
	static uint8_t texels[QR_SIDE_MAX * QR_SIDE_MAX];
	strncpy (qr_text, text, sizeof qr_text - 1);
	qr_side = 0;
	if (!qrcodegen_encodeText (text, temp, code, qrcodegen_Ecc_MEDIUM, 1, QR_VERSION_MAX, qrcodegen_Mask_AUTO, true))
	{
		printf ("wifi: no QR code for %u characters\n", (unsigned) strlen (text));
		return;
	}
	int n = qrcodegen_getSize (code), side = n + 2 * QR_QUIET;
	for (int y = 0; y < side; y++)			/* (the texture's rows go up: the code's last row first) */
	{
		for (int x = 0; x < side; x++)
		{
			int cx = x - QR_QUIET, cy = n - 1 - (y - QR_QUIET);
			texels[y * side + x] = cx >= 0 && cy >= 0 && cx < n && cy < n && qrcodegen_getModule (code, cx, cy) ? 0 : 255;
		}
	}
	glBindTexture (GL_TEXTURE_2D, qr_texture);
	glPixelStorei (GL_UNPACK_ALIGNMENT, 1);
	glTexImage2D (GL_TEXTURE_2D, 0, GL_LUMINANCE, side, side, 0, GL_LUMINANCE, GL_UNSIGNED_BYTE, texels);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	qr_side = side;
	printf ("wifi: a QR code of %d modules a side\n", n);
}

/* the code at the top of the screen, whole pixels a module; returns where it ends (pixels from the top) */
static int qr_draw (const GLint vp[4])
{
	int pixels = vp[3] * 62 / 100 / qr_side;
	pixels = pixels < 1 ? 1 : pixels;
	int size = pixels * qr_side, x = (vp[2] - size) / 2, top = vp[3] * 3 / 100;
	glUseProgram (view);
	glUniform4f (v_rect, 2.0f * x / vp[2] - 1.0f, 1.0f - 2.0f * (top + size) / vp[3], 2.0f * size / vp[2], 2.0f * size / vp[3]);
	glUniform1f (v_mode, 1.0f);
	glActiveTexture (GL_TEXTURE0);
	glBindTexture (GL_TEXTURE_2D, qr_texture);
	glBindBuffer (GL_ARRAY_BUFFER, quad);
	glEnableVertexAttribArray (v_pos);
	glVertexAttribPointer (v_pos, 2, GL_FLOAT, GL_FALSE, 0, (void *) 0);
	glDrawArrays (GL_TRIANGLES, 0, 6);
	return top + size;
}

/* a line in the middle of the screen, in the HUD's capitals */
static float line (const GLint vp[4], float y, float scale, uint32_t color, const char *text)
{
	char upper[40];
	unsigned n = 0;
	for (; text[n] && n < sizeof upper - 1; n++)
	{
		upper[n] = text[n] >= 'a' && text[n] <= 'z' ? (char) (text[n] - 32) : text[n];
	}
	upper[n] = '\0';
	hud_text_scaled ((float) (int) ((vp[2] - n * HUD_CHAR_W * scale) / 2), y, upper, color, scale);
	return y + (HUD_CHAR_H + 3) * scale;
}

/* seconds since 1970 as "2026-10-01 21:03:05" */
static void date (uint32_t t, char *out, unsigned max)
{
	uint32_t days = t / 86400, s = t % 86400;
	int z = (int) days + 719468, era = z / 146097, doe = z - era * 146097;
	int yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365, y = yoe + era * 400;
	int doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153;
	int d = doy - (153 * mp + 2) / 5 + 1, m = mp < 10 ? mp + 3 : mp - 9;
	snprintf (out, max, "%04d-%02d-%02d %02u:%02u:%02u", y + (m <= 2), m, d, (unsigned) (s / 3600), (unsigned) (s / 60 % 60),
		  (unsigned) (s % 60));
}

int main (void)
{
	stdio_init_all ();
	gpio_init (SETUP_PIN);
	gpio_set_dir (SETUP_PIN, GPIO_IN);
	gpio_pull_up (SETUP_PIN);
	pgpu_init ();
	printf ("\nwifi: waiting for the RPi (READY)...\n");
	while (!pgpu_wait_ready (1000))
	{
	}
	pgpu_set_reply_phase (1);
	int tries = 0;
	while (!pglInit () && ++tries < 5)		/* the first reply can be missed */
	{
	}
	if (!hud_init ())
	{
		printf ("wifi: the HUD program didn't link\n");
	}
	static const float square[12] = {0, 0, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1};
	view = glCreateProgram ();
	glProgramBinaryOES (view, PGL_PROGRAM_BINARY_PGPU, &texview_info, sizeof texview_info);
	v_rect = glGetUniformLocation (view, "u_rect");
	v_mode = glGetUniformLocation (view, "u_mode");
	v_pos = glGetAttribLocation (view, "a_pos");
	glUseProgram (view);
	glUniform1i (glGetUniformLocation (view, "u_tex"), 0);
	glUniform1i (glGetUniformLocation (view, "u_cube"), 1);		/* (not the 2D one's unit: GL ES won't draw) */
	glGenBuffers (1, &quad);
	glBindBuffer (GL_ARRAY_BUFFER, quad);
	glBufferData (GL_ARRAY_BUFFER, sizeof square, square, GL_STATIC_DRAW);
	glGenTextures (1, &qr_texture);
	glDisable (GL_DEPTH_TEST);
	glDisable (GL_CULL_FACE);

	bool setup = !gpio_get (SETUP_PIN);
	printf ("wifi: %s; \"s\" on the console: set up again\n",
		setup ? "the stick's button is held: set up" : "the stick's button held at the start: set up again");
	wifi_setup_start (setup);

	GLint vp[4] = {0};
	while (true)
	{
		screen_update ("wifi", vp);
		if (getchar_timeout_us (0) == 's')
		{
			printf ("wifi: set up again\n");
			wifi_setup_again ();
		}
		wifi_setup_poll ();
		const wifi_status_t *w = wifi_setup_status ();

		char text[96];
		bool code = wifi_setup_qr (text, sizeof text);
		if (code && strcmp (text, qr_text) != 0)
		{
			qr_make (text);
		}

		glViewport (vp[0], vp[1], vp[2], vp[3]);
		glClearColor (0.07f, 0.08f, 0.10f, 1.0f);
		glClear (GL_COLOR_BUFFER_BIT);
		float hs = vp[3] >= 480 ? (float) (vp[3] / 240) : 1.0f;
		float y = vp[3] * 0.30f;
		if (code && qr_side)
		{
			y = qr_draw (vp) + 4 * hs;
		}
		char s[48];
		hud_begin ();
		switch (w->state)
		{
		case WIFI_LOOKING:
			y = line (vp, y, hs, WHITE, "WIFI");
			line (vp, y, hs, GREY, "LOOKING FOR NETWORKS");
			break;
		case WIFI_JOINING:
			y = line (vp, y, hs, WHITE, "JOINING");
			line (vp, y, hs, YELLOW, w->ssid);
			break;
		case WIFI_AP:
			y = line (vp, y, hs, WHITE, "SCAN TO SETUP WIFI");
			snprintf (s, sizeof s, "%s  %s", w->ap_ssid, w->ap_password);
			y = line (vp, y, hs, GREY, s);
			line (vp, y, hs * 0.5f, RED, w->note);
			break;
		case WIFI_AP_PHONE:
			y = line (vp, y, hs, GREEN, "PHONE CONNECTED");
			snprintf (s, sizeof s, "OPEN %s", wifi_setup_url () + 7);
			y = line (vp, y, hs, WHITE, s);
			line (vp, y, hs * 0.5f, RED, w->note);
			break;
		case WIFI_ONLINE:
			y = line (vp, y, hs, GREEN, "WIFI OK");
			y = line (vp, y, hs, YELLOW, w->ssid);
			snprintf (s, sizeof s, "IP %u.%u.%u.%u", w->ip[0], w->ip[1], w->ip[2], w->ip[3]);
			y = line (vp, y, hs, WHITE, s);
			if (w->rssi)
			{
				snprintf (s, sizeof s, "SIGNAL %d DBM", w->rssi);
				y = line (vp, y, hs, GREY, s);
			}
			if (w->time)
			{
				date (w->time, s, sizeof s);
				y = line (vp, y, hs, GREY, s);
				line (vp, y, hs * 0.5f, GREY, "UTC FROM THE NET");
			}
			break;
		case WIFI_FAILED:
			line (vp, y, hs, RED, "NO RADIO");
			break;
		default:
			break;
		}
		hud_end ();
		hud_draw ();
		pglSwapBuffers ();
		pgpu_wait_frame (100);			/* pace on the screen */
	}
}

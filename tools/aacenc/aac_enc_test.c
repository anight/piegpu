/*
 * aac_enc_test.c - the RPi's AAC encoder (gpu/audio/aac_enc.c) on a PC:
 *
 *   aac_enc_test mdct                    its fast MDCT against the definition
 *   aac_enc_test RATE in.pcm out.aac     16-bit stereo frames to an ADTS file
 *                                        (ffmpeg decodes that: compare.py)
 *
 *   cc -O2 -I ../../gpu/audio -o aac_enc_test aac_enc_test.c ../../gpu/audio/aac_enc.c -lm
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#define aac_enc_frame aac_enc_frame_	/* (the encoder's own source, for its transform) */
#include "../../gpu/audio/aac_enc.c"
#undef aac_enc_frame

static int test_mdct (void)
{
	aac_enc_t *e = aac_enc_open (48000);
	static float before[M], now[M], fast[M];
	srand (1);
	for (unsigned i = 0; i < M; i++)
	{
		before[i] = (float) (rand () % 65536 - 32768);
		now[i] = (float) (rand () % 65536 - 32768);
	}
	mdct (e, before, now, fast);
	double worst = 0, largest = 0;
	for (unsigned k = 0; k < M; k++)
	{
		double sum = 0;
		for (unsigned n = 0; n < N; n++)
		{
			double x = n < M ? before[n] : now[n - M];
			double w = sin (M_PI * (n + 0.5) / N);
			sum += x * w * cos (2 * M_PI / N * (n + 0.5 + N / 4.0) * (k + 0.5));
		}
		sum *= 2;
		worst = fabs (sum - fast[k]) > worst ? fabs (sum - fast[k]) : worst;
		largest = fabs (sum) > largest ? fabs (sum) : largest;
	}
	printf ("mdct: the largest coefficient %.0f, the fast one off by %.3f at most (%.1e of it)\n", largest, worst, worst / largest);
	return worst / largest < 1e-4 ? 0 : 1;
}

int main (int argc, char **argv)
{
	if (argc == 2 && strcmp (argv[1], "mdct") == 0)
	{
		return test_mdct ();
	}
	if (argc != 4)
	{
		fprintf (stderr, "aac_enc_test mdct | RATE in.pcm out.aac\n");
		return 2;
	}
	unsigned rate = (unsigned) atoi (argv[1]);
	aac_enc_t *e = aac_enc_open (rate);
	FILE *in = fopen (argv[2], "rb"), *out = fopen (argv[3], "wb");
	if (!e || !in || !out)
	{
		fprintf (stderr, "can't\n");
		return 1;
	}
	uint8_t asc[2];
	aac_enc_config (e, asc);
	static int16_t frames[2 * AAC_ENC_FRAMES];
	static uint8_t unit[AAC_ENC_MAX_BYTES];
	unsigned units = 0, most = 0, last = 0;
	unsigned long total = 0;
	clock_t start = clock ();
	for (;;)
	{
		memset (frames, 0, sizeof frames);
		size_t n = fread (frames, 4, AAC_ENC_FRAMES, in);
		unsigned bytes = n || !last ? aac_enc_frame_ (e, n ? frames : NULL, unit) : 0;
		last = !n;
		if (bytes)
		{
			unsigned len = bytes + 7, index = (asc[0] & 7) << 1 | asc[1] >> 7;
			const uint8_t header[7] =
			{
				0xFF, 0xF1, (uint8_t) (1 << 6 | index << 2 | 0), (uint8_t) (2 << 6 | len >> 11), (uint8_t) (len >> 3),
				(uint8_t) ((len & 7) << 5 | 0x1F), 0xFC,
			};
			fwrite (header, 1, 7, out);
			fwrite (unit, 1, bytes, out);
			units++;
			total += bytes;
			most = bytes > most ? bytes : most;
		}
		if (!n)
		{
			break;
		}
	}
	double seconds = (double) units * AAC_ENC_FRAMES / rate, took = (double) (clock () - start) / CLOCKS_PER_SEC;
	printf ("%u units, %.1f s: %.0f kbit/s, a unit at most %u bytes; encoded in %.3f s (%.0f times as fast as it plays)\n",
		units, seconds, total * 8 / seconds / 1000, most, took, seconds / took);
	return 0;
}

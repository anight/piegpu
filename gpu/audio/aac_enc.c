/*
 * aac_enc.c - see aac_enc.h
 */
#include "aac_enc.h"
#include "aac_enc_tables.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define N		2048			/* a block: two units' frames, half of it new each time */
#define M		1024			/* its coefficients */
#define BANDS		44			/* scale factor bands coded: to 18.7 kHz at 48 kHz */
#define SF_OFFSET	100
#define PI_F		3.14159265358979f

/* the scale factor bands of long blocks at 44.1 and 48 kHz (ISO 14496-3, table 4.110) */
static const uint16_t band_start[50] =
{
	0, 4, 8, 12, 16, 20, 24, 28, 32, 36, 40, 48, 56, 64, 72, 80, 88, 96, 108, 120, 132, 144, 160, 176, 196, 216,
	240, 264, 292, 320, 352, 384, 416, 448, 480, 512, 544, 576, 608, 640, 672, 704, 736, 768, 800, 832, 864, 896,
	928, 1024,
};

static const uint16_t *const cb_code[12] =
{
	0, aac_enc_code_1, aac_enc_code_2, aac_enc_code_3, aac_enc_code_4, aac_enc_code_5, aac_enc_code_6,
	aac_enc_code_7, aac_enc_code_8, aac_enc_code_9, aac_enc_code_10, aac_enc_code_11,
};
static const uint8_t *const cb_bits[12] =
{
	0, aac_enc_bits_1, aac_enc_bits_2, aac_enc_bits_3, aac_enc_bits_4, aac_enc_bits_5, aac_enc_bits_6,
	aac_enc_bits_7, aac_enc_bits_8, aac_enc_bits_9, aac_enc_bits_10, aac_enc_bits_11,
};
/* a codebook's values: the largest, signed in the codeword or by bits after it */
static const uint8_t cb_largest[12] = {0, 1, 1, 2, 2, 4, 4, 7, 7, 12, 12, 16};
static const uint8_t cb_signed[12] = {0, 1, 1, 0, 0, 1, 1, 0, 0, 0, 0, 0};

struct aac_enc_s
{
	unsigned rate_index;
	int primed;
	float window[M];			/* the sine window's first half (the second: mirrored) */
	float twist[M / 2][2];			/* e^(-i pi (n + 1/8) / M): before and after the FFT */
	float root[M / 4][2];			/* the FFT's: e^(-2 pi i n / (M / 2)) */
	uint16_t reversed[M / 2];
	float gain[256];			/* 2^(-3/16 (sf - 100)): the quantiser's, by scale factor */
	float allowed[BANDS];			/* the noise let into a band, of its energy */
	float before[2][M];			/* the frames given the last time */
	/* a unit's working: the spectra, their 3/4 powers, the quantised values */
	float spectrum[2][M], power34[2][M];
	int16_t quantised[2][M];
	uint8_t sf[2][BANDS], cb[2][BANDS];
	/* the bits */
	uint8_t *out;
	unsigned n_bits;
};

/* ---- the bits ------------------------------------------------------------------------ */

static void put (aac_enc_t *e, uint32_t value, unsigned bits)
{
	while (bits)
	{
		unsigned room = 8 - (e->n_bits & 7), n = bits < room ? bits : room;
		uint8_t *byte = &e->out[e->n_bits >> 3];
		if (room == 8)
		{
			*byte = 0;
		}
		*byte |= (uint8_t) (((value >> (bits - n)) & ((1u << n) - 1)) << (room - n));
		e->n_bits += n;
		bits -= n;
	}
}

/* ---- the transform -------------------------------------------------------------------
 *
 * The MDCT of a block of four quarters (a, b, c, d) is the DCT-IV of (-c
 * reversed - d, a - b reversed); and a DCT-IV of M values is an FFT of M / 2
 * complex ones, twisted before and after. AAC's has a factor of 2.
 */

static void fft (const aac_enc_t *e, float (*x)[2])		/* M / 2 values, in place (given bit-reversed) */
{
	for (unsigned half = 1; half < M / 2; half *= 2)
	{
		unsigned step = M / 4 / half;
		for (unsigned i = 0; i < M / 2; i += 2 * half)
		{
			for (unsigned k = 0; k < half; k++)
			{
				const float *w = e->root[k * step];
				float *a = x[i + k], *b = x[i + k + half];
				float re = b[0] * w[0] - b[1] * w[1], im = b[0] * w[1] + b[1] * w[0];
				b[0] = a[0] - re;
				b[1] = a[1] - im;
				a[0] += re;
				a[1] += im;
			}
		}
	}
}

/* a block (the frames before, then these), windowed, to its M coefficients */
static void mdct (const aac_enc_t *e, const float *before, const float *now, float *out)
{
	static float folded[M], work[M / 2][2];			/* (one encoder at a time) */
	/* z[n] = block[n] window[n]; the block's quarters: a, b = before; c, d = now */
	for (unsigned n = 0; n < M / 2; n++)
	{
		/* -c reversed - d */
		folded[n] = -now[M / 2 - 1 - n] * e->window[M / 2 + n] - now[M / 2 + n] * e->window[M / 2 - 1 - n];
		/* a - b reversed */
		folded[M / 2 + n] = before[n] * e->window[n] - before[M - 1 - n] * e->window[M - 1 - n];
	}
	for (unsigned n = 0; n < M / 2; n++)
	{
		float re = folded[2 * n], im = folded[M - 1 - 2 * n];
		float *t = work[e->reversed[n]];
		t[0] = re * e->twist[n][0] - im * e->twist[n][1];
		t[1] = re * e->twist[n][1] + im * e->twist[n][0];
	}
	fft (e, work);
	for (unsigned k = 0; k < M / 2; k++)
	{
		float re = work[k][0] * e->twist[k][0] - work[k][1] * e->twist[k][1];
		float im = work[k][0] * e->twist[k][1] + work[k][1] * e->twist[k][0];
		out[2 * k] = 2.0f * re;
		out[M - 1 - 2 * k] = -2.0f * im;
	}
}

/* ---- a channel's coefficients to numbers -------------------------------------------- */

static int quantise_band (aac_enc_t *e, unsigned ch, unsigned band, unsigned sf)
{
	float k = e->gain[sf];
	int largest = 0;
	for (unsigned i = band_start[band]; i < band_start[band + 1]; i++)
	{
		int q = (int) (e->power34[ch][i] * k + 0.4054f);
		largest = q > largest ? q : largest;
		e->quantised[ch][i] = (int16_t) (e->spectrum[ch][i] < 0 ? -q : q);
	}
	return largest;
}

/* the bits a band's numbers take in a codebook */
static unsigned band_bits (const aac_enc_t *e, unsigned ch, unsigned band, unsigned cb)
{
	const int16_t *q = e->quantised[ch];
	const uint8_t *bits = cb_bits[cb];
	unsigned total = 0, largest = cb_largest[cb];
	if (cb < 5)
	{
		for (unsigned i = band_start[band]; i < band_start[band + 1]; i += 4)
		{
			if (cb_signed[cb])
			{
				total += bits[27 * (q[i] + 1) + 9 * (q[i + 1] + 1) + 3 * (q[i + 2] + 1) + q[i + 3] + 1];
			}
			else
			{
				total += bits[27 * abs (q[i]) + 9 * abs (q[i + 1]) + 3 * abs (q[i + 2]) + abs (q[i + 3])]
					 + (q[i] != 0) + (q[i + 1] != 0) + (q[i + 2] != 0) + (q[i + 3] != 0);
			}
		}
		return total;
	}
	for (unsigned i = band_start[band]; i < band_start[band + 1]; i += 2)
	{
		if (cb_signed[cb])
		{
			total += bits[9 * (q[i] + 4) + q[i + 1] + 4];
			continue;
		}
		unsigned a = (unsigned) abs (q[i]), b = (unsigned) abs (q[i + 1]);
		total += (a != 0) + (b != 0);
		if (cb == 11)
		{
			for (unsigned v = a, twice = 0; twice < 2; v = b, twice++)
			{
				if (v >= 16)			/* an escape: n - 4 ones, a zero, n bits */
				{
					unsigned n = 4;
					while (v >> (n + 1))
					{
						n++;
					}
					total += 2 * n - 3;
				}
			}
			a = a > 16 ? 16 : a;
			b = b > 16 ? 16 : b;
		}
		total += bits[(largest + 1) * a + b];
	}
	return total;
}

static void put_band (aac_enc_t *e, unsigned ch, unsigned band, unsigned cb)
{
	const int16_t *q = e->quantised[ch];
	const uint16_t *code = cb_code[cb];
	const uint8_t *bits = cb_bits[cb];
	unsigned largest = cb_largest[cb], step = cb < 5 ? 4 : 2;
	for (unsigned i = band_start[band]; i < band_start[band + 1]; i += step)
	{
		unsigned index = 0;
		for (unsigned j = 0; j < step; j++)
		{
			int v = q[i + j];
			unsigned a = (unsigned) abs (v);
			index = index * (cb_signed[cb] ? 2 * largest + 1 : largest + 1)
				+ (cb_signed[cb] ? (unsigned) (v + (int) largest) : a > 16 && cb == 11 ? 16 : a);
		}
		put (e, code[index], bits[index]);
		if (!cb_signed[cb])
		{
			for (unsigned j = 0; j < step; j++)
			{
				if (q[i + j])
				{
					put (e, q[i + j] < 0, 1);
				}
			}
		}
		if (cb == 11)
		{
			for (unsigned j = 0; j < step; j++)
			{
				unsigned v = (unsigned) abs (q[i + j]);
				if (v >= 16)
				{
					unsigned n = 4;
					while (v >> (n + 1))
					{
						n++;
					}
					put (e, (1u << (n - 3)) - 2, n - 3);		/* n - 4 ones, a zero */
					put (e, v - (1u << n), n);
				}
			}
		}
	}
}

/* a channel's bands quantised, coarser by `coarser` scale factor steps than
   the noise allowed asks; a codebook chosen for each. The bits they'll take */
static unsigned prepare_channel (aac_enc_t *e, unsigned ch, int coarser)
{
	unsigned total = 8 + 3;				/* global gain; no pulses, no TNS, no gain control */
	int last_sf = -1;
	unsigned section_cb = 16;
	unsigned section_len = 0;
	for (unsigned band = 0; band < BANDS; band++)
	{
		unsigned from = band_start[band], to = band_start[band + 1];
		float energy = 0, roots = 0, most = 0;
		for (unsigned i = from; i < to; i++)
		{
			float a = fabsf (e->spectrum[ch][i]);
			energy += a * a;
			roots += sqrtf (a);
			most = e->power34[ch][i] > most ? e->power34[ch][i] : most;
		}
		unsigned cb = 0, sf = 0;
		/* the noise let in, a coefficient: so far under the band's level, and never
		   under what 16 bits had anyway. The quantiser's noise at a step s is
		   4/27 mean (sqrt |x|) s^(3/2), which gives the step, which is 2^((sf - 100) / 4) */
		float noise = energy / (float) (to - from) * e->allowed[band];
		noise = noise < 600.0f ? 600.0f : noise;
		if (roots > 0)
		{
			float want = SF_OFFSET + (8.0f / 3.0f) * log2f (6.75f * noise * (float) (to - from) / roots) + (float) coarser;
			int s = (int) floorf (want + 0.5f);
			if (last_sf >= 0)				/* (a difference is coded: 60 either way at most) */
			{
				s = s > last_sf + 60 ? last_sf + 60 : s < last_sf - 60 ? last_sf - 60 : s;
			}
			s = s < 0 ? 0 : s > 255 ? 255 : s;
			while (most * e->gain[s] + 0.4054f >= 8192.0f && s < 255 && (last_sf < 0 || s < last_sf + 60))
			{
				s++;
			}
			sf = (unsigned) s;
			int largest = quantise_band (e, ch, band, sf);
			if (largest > 8191)				/* (can't be said: silence there, rather) */
			{
				largest = 0;
			}
			if (largest)
			{
				unsigned first = largest <= 1 ? 1 : largest <= 2 ? 3 : largest <= 4 ? 5 : largest <= 7 ? 7 : largest <= 12 ? 9 : 11;
				unsigned a = band_bits (e, ch, band, first), b = first < 11 ? band_bits (e, ch, band, first + 1) : a;
				cb = b < a ? first + 1 : first;
				total += b < a ? b : a;
				total += aac_enc_bits_sf[(int) sf - (last_sf < 0 ? (int) sf : last_sf) + 60];
				last_sf = (int) sf;
			}
		}
		if (!cb)
		{
			memset (&e->quantised[ch][from], 0, (to - from) * sizeof (int16_t));
		}
		e->cb[ch][band] = (uint8_t) cb;
		e->sf[ch][band] = (uint8_t) sf;
		if (cb != section_cb || section_len == 31)		/* a section: its codebook, its length */
		{
			total += 4 + 5 + (section_len == 31 && cb == section_cb ? 0 : 0);
			section_cb = cb;
			section_len = 0;
		}
		section_len++;
	}
	return total + 16;			/* (the sections' lengths that run past 30 bands: a few bits more) */
}

static void put_channel (aac_enc_t *e, unsigned ch)
{
	unsigned gain = 0;
	for (unsigned band = 0; band < BANDS; band++)
	{
		if (e->cb[ch][band])
		{
			gain = e->sf[ch][band];
			break;
		}
	}
	put (e, gain, 8);
	/* the sections: runs of bands with one codebook; a length of 31 says "and more" */
	for (unsigned band = 0; band < BANDS; )
	{
		unsigned len = 1;
		while (band + len < BANDS && e->cb[ch][band + len] == e->cb[ch][band])
		{
			len++;
		}
		put (e, e->cb[ch][band], 4);
		for (unsigned left = len; ; left -= 31)
		{
			put (e, left < 31 ? left : 31, 5);
			if (left < 31)
			{
				break;
			}
		}
		band += len;
	}
	unsigned last = gain;
	for (unsigned band = 0; band < BANDS; band++)
	{
		if (e->cb[ch][band])
		{
			unsigned index = e->sf[ch][band] + 60 - last;
			put (e, aac_enc_code_sf[index], aac_enc_bits_sf[index]);
			last = e->sf[ch][band];
		}
	}
	put (e, 0, 3);					/* no pulses, no TNS, no gain control */
	for (unsigned band = 0; band < BANDS; band++)
	{
		if (e->cb[ch][band])
		{
			put_band (e, ch, band, e->cb[ch][band]);
		}
	}
}

/* ---- the encoder --------------------------------------------------------------------- */

aac_enc_t *aac_enc_open (unsigned rate)
{
	if (rate != 48000 && rate != 44100)
	{
		return NULL;
	}
	aac_enc_t *e = calloc (1, sizeof *e);
	if (!e)
	{
		return NULL;
	}
	e->rate_index = rate == 48000 ? 3 : 4;
	for (unsigned n = 0; n < M; n++)
	{
		e->window[n] = sinf (PI_F * ((float) n + 0.5f) / (float) N);
	}
	for (unsigned n = 0; n < M / 2; n++)
	{
		float a = -PI_F * ((float) n + 0.125f) / (float) M;
		e->twist[n][0] = cosf (a);
		e->twist[n][1] = sinf (a);
		unsigned r = 0;
		for (unsigned bit = 0; bit < 9; bit++)			/* M / 2 = 512: 9 bits */
		{
			r |= ((n >> bit) & 1) << (8 - bit);
		}
		e->reversed[n] = (uint16_t) r;
	}
	for (unsigned n = 0; n < M / 4; n++)
	{
		float a = -2.0f * PI_F * (float) n / (float) (M / 2);
		e->root[n][0] = cosf (a);
		e->root[n][1] = sinf (a);
	}
	for (unsigned sf = 0; sf < 256; sf++)
	{
		e->gain[sf] = powf (2.0f, -0.1875f * ((float) sf - SF_OFFSET));
	}
	for (unsigned band = 0; band < BANDS; band++)
	{
		/* the noise under a band's level: 27 dB up to 4 kHz or so (band 24), then
		   less with every band, to 14 dB in the last (a tone hides noise that far under it; noise hides much more) */
		float db = band <= 24 ? 27.0f : 27.0f - 13.0f * (float) (band - 24) / (float) (BANDS - 1 - 24);
		e->allowed[band] = powf (10.0f, -db / 10.0f);
	}
	return e;
}

void aac_enc_close (aac_enc_t *e)
{
	free (e);
}

void aac_enc_config (const aac_enc_t *e, uint8_t asc[2])
{
	/* AAC LC (2: 5 bits), the rate's index (4), 2 channels (4), three zero bits */
	asc[0] = (uint8_t) (2 << 3 | e->rate_index >> 1);
	asc[1] = (uint8_t) ((e->rate_index & 1) << 7 | 2 << 3);
}

unsigned aac_enc_frame (aac_enc_t *e, const int16_t *frames, uint8_t *out)
{
	static float now[2][M];
	for (unsigned i = 0; i < M; i++)
	{
		now[0][i] = frames ? (float) frames[2 * i] : 0.0f;
		now[1][i] = frames ? (float) frames[2 * i + 1] : 0.0f;
	}
	if (!e->primed)
	{
		memcpy (e->before, now, sizeof now);
		e->primed = 1;
		return 0;
	}
	for (unsigned ch = 0; ch < 2; ch++)
	{
		mdct (e, e->before[ch], now[ch], e->spectrum[ch]);
	}
	memcpy (e->before, now, sizeof now);

	/* mid and side instead of left and right, when they are much alike (the side then is next to nothing) */
	float left = 0, right = 0, side = 0;
	for (unsigned i = 0; i < band_start[BANDS]; i++)
	{
		float l = e->spectrum[0][i], r = e->spectrum[1][i];
		left += l * l;
		right += r * r;
		side += (l - r) * (l - r) * 0.25f;
	}
	int mid_side = side < 0.3f * (left < right ? left : right);
	for (unsigned i = 0; i < M; i++)
	{
		if (mid_side)
		{
			float l = e->spectrum[0][i], r = e->spectrum[1][i];
			e->spectrum[0][i] = 0.5f * (l + r);
			e->spectrum[1][i] = 0.5f * (l - r);
		}
		for (unsigned ch = 0; ch < 2; ch++)
		{
			float a = fabsf (e->spectrum[ch][i]);
			e->power34[ch][i] = sqrtf (a * sqrtf (a));
		}
	}

	/* as fine as the noise allowed asks; coarser if that is more than a unit may hold */
	for (int coarser = 0; ; coarser += 2)
	{
		unsigned bits = 3 + 4 + 1 + 11 + 2 + 3 + 7;		/* the pair's head, the end, to a whole byte */
		bits += prepare_channel (e, 0, coarser) + prepare_channel (e, 1, coarser);
		if (bits <= AAC_ENC_MAX_BYTES * 8 || coarser > 120)
		{
			break;
		}
	}

	e->out = out;
	e->n_bits = 0;
	put (e, 1, 3);				/* a channel pair */
	put (e, 0, 4);				/* its tag */
	put (e, 1, 1);				/* one window for both */
	put (e, 0, 1);				/* (reserved) */
	put (e, 0, 2);				/* long blocks only */
	put (e, 0, 1);				/* the sine window */
	put (e, BANDS, 6);
	put (e, 0, 1);				/* no prediction */
	put (e, mid_side ? 2 : 0, 2);		/* mid/side: in every band, or in none */
	put_channel (e, 0);
	put_channel (e, 1);
	put (e, 7, 3);				/* the end */
	if (e->n_bits & 7)
	{
		put (e, 0, 8 - (e->n_bits & 7));
	}
	return e->n_bits >> 3;
}

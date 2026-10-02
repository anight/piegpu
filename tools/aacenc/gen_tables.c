/*
 * gen_tables.c - the AAC encoder's Huffman tables (gpu/audio/aac_enc_tables.h),
 * made from the decoder's: FAAD2's codebooks (gpu/audio/faad2/libfaad/codebook)
 * (all included by its hcb.h) are walked, every codeword found, and written down by the values it stands
 * for. Checked on the way: every value of a codebook has one codeword, and
 * the codewords fill the code space (Kraft's sum is 1).
 *
 *   cc -I ../../gpu/audio/faad2/libfaad -o gen_tables gen_tables.c && ./gen_tables > ../../gpu/audio/aac_enc_tables.h
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "codebook/hcb.h"

#define MAX_SYMBOLS	289

static uint32_t code[MAX_SYMBOLS];
static int bits[MAX_SYMBOLS];
static int symbols;

static void found (int index, uint32_t c, int n)
{
	if (index < 0 || index >= symbols)
	{
		fprintf (stderr, "a value outside its codebook (%d)\n", index);
		exit (1);
	}
	if (bits[index] && (bits[index] != n || code[index] != c))
	{
		fprintf (stderr, "two codewords for %d\n", index);
		exit (1);
	}
	code[index] = c;
	bits[index] = n;
}

/* a codebook's value: how far its numbers go, signed or not, four or two of them */
static int index_of (const int8_t *v, int n, int largest, int is_signed)
{
	int index = 0, base = is_signed ? 2 * largest + 1 : largest + 1;
	for (int i = 0; i < n; i++)
	{
		int x = is_signed ? v[i] + largest : v[i];
		if (x < 0 || x >= base)
			return -1;
		index = index * base + x;
	}
	return index;
}

static void write_out (const char *name, int largest, int is_signed, int n)
{
	double kraft = 0;
	int longest = 0;
	for (int i = 0; i < symbols; i++)
	{
		if (!bits[i])
		{
			fprintf (stderr, "%s: no codeword for %d\n", name, i);
			exit (1);
		}
		kraft += 1.0 / (double) (1u << bits[i]);
		longest = bits[i] > longest ? bits[i] : longest;
	}
	if (kraft < 0.999999 || kraft > 1.000001)
	{
		fprintf (stderr, "%s: Kraft's sum is %f\n", name, kraft);
		exit (1);
	}
	if (n)
		printf ("/* %s: %d values a codeword, %s%d .. %d; %d codewords, the longest %d bits */\n", name, n,
			is_signed ? "-" : "", is_signed ? largest : 0, largest, symbols, longest);
	else
		printf ("/* %s: %d codewords, the longest %d bits */\n", name, symbols, longest);
	printf ("static const %s aac_enc_code_%s[%d] =\n{", longest > 16 ? "uint32_t" : "uint16_t", name, symbols);
	for (int i = 0; i < symbols; i++)
		printf ("%s0x%X,", i % 12 ? " " : "\n\t", code[i]);
	printf ("\n};\nstatic const uint8_t aac_enc_bits_%s[%d] =\n{", name, symbols);
	for (int i = 0; i < symbols; i++)
		printf ("%s%d,", i % 24 ? " " : "\n\t", bits[i]);
	printf ("\n};\n\n");
}

static void start (int n)
{
	symbols = n;
	memset (bits, 0, sizeof bits);
	memset (code, 0, sizeof code);
}

/* the decoder's tables of two steps: root_bits bits pick a first entry, which says how many more pick the second */
#define TWO_STEP(name, t1, t2, root_bits, n, largest, is_signed)					\
	{												\
		int base = (is_signed) ? 2 * (largest) + 1 : (largest) + 1, count = 1;			\
		for (int i = 0; i < (n); i++) count *= base;						\
		start (count);										\
		for (unsigned i = 0; i < 1u << (root_bits); i++)					\
		{											\
			unsigned extra = t1[i].extra_bits;						\
			for (unsigned e = 0; e < 1u << extra; e++)					\
			{										\
				int8_t v[4] = {t2[t1[i].offset + e].x, t2[t1[i].offset + e].y, 0, 0};	\
				if ((n) == 4) { v[2] = QUAD_V (t2[t1[i].offset + e]); v[3] = QUAD_W (t2[t1[i].offset + e]); }	\
				unsigned len = t2[t1[i].offset + e].bits, total = (root_bits) + extra;	\
				if (len > total) { fprintf (stderr, #name ": a codeword longer than its bits\n"); exit (1); }	\
				found (index_of (v, (n), (largest), (is_signed)), (i << extra | e) >> (total - len), len);	\
			}										\
		}											\
		write_out (#name, (largest), (is_signed), (n));						\
	}

/* the decoder's trees: a node's two ways lead on by so many entries, a leaf has the values */
#define TREE(name, table, n, largest, is_signed)							\
	{												\
		int base = (is_signed) ? 2 * (largest) + 1 : (largest) + 1, count = 1;			\
		for (int i = 0; i < (n); i++) count *= base;						\
		start (count);										\
		struct { unsigned at; uint32_t c; int len; } stack[64];				\
		int depth = 0;										\
		stack[depth].at = 0; stack[depth].c = 0; stack[depth++].len = 0;			\
		while (depth)										\
		{											\
			unsigned at = stack[--depth].at; uint32_t c = stack[depth].c; int len = stack[depth].len;	\
			if (table[at].is_leaf)								\
			{										\
				int8_t v[4] = {0, 0, 0, 0};						\
				for (int i = 0; i < (n); i++) v[i] = table[at].data[i];		\
				found (index_of (v, (n), (largest), (is_signed)), c, len);		\
			}										\
			else										\
			{										\
				for (int b = 0; b < 2; b++)						\
				{									\
					stack[depth].at = at + table[at].data[b];			\
					stack[depth].c = c << 1 | b;					\
					stack[depth++].len = len + 1;					\
				}									\
			}										\
		}											\
		write_out (#name, (largest), (is_signed), (n));						\
	}

int main (void)
{
	printf ("/*\n * aac_enc_tables.h - the AAC encoder's Huffman codes: a codebook's codeword and its length\n"
		" * by the values it stands for (the first of them counts most: four of -1 .. 1 are\n"
		" * 27 (w + 1) + 9 (x + 1) + 3 (y + 1) + (z + 1)). Made by tools/aacenc/gen_tables.c from\n"
		" * FAAD2's tables: not to be changed here.\n */\n\n");
#define QUAD_V(e)	((e).v)
#define QUAD_W(e)	((e).w)
	TWO_STEP (1, hcb1_1, hcb1_2, 5, 4, 1, 1)
	TWO_STEP (2, hcb2_1, hcb2_2, 5, 4, 1, 1)
	TREE (3, hcb3, 4, 2, 0)
	TWO_STEP (4, hcb4_1, hcb4_2, 5, 4, 2, 0)
#undef QUAD_V
#undef QUAD_W
#define QUAD_V(e)	0
#define QUAD_W(e)	0
	TREE (5, hcb5, 2, 4, 1)
	TWO_STEP (6, hcb6_1, hcb6_2, 5, 2, 4, 1)
	TREE (7, hcb7, 2, 7, 0)
	TWO_STEP (8, hcb8_1, hcb8_2, 5, 2, 7, 0)
	TREE (9, hcb9, 2, 12, 0)
	TWO_STEP (10, hcb10_1, hcb10_2, 6, 2, 12, 0)
	TWO_STEP (11, hcb11_1, hcb11_2, 5, 2, 16, 0)

	/* the scale factors': a tree of pairs (the ways on; a leaf: its value, 0) */
	start (121);
	struct { unsigned at; uint32_t c; int len; } stack[64];
	int depth = 0;
	stack[depth].at = 0; stack[depth].c = 0; stack[depth++].len = 0;
	while (depth)
	{
		unsigned at = stack[--depth].at; uint32_t c = stack[depth].c; int len = stack[depth].len;
		if (!hcb_sf[at][1])
			found (hcb_sf[at][0], c, len);
		else
			for (int b = 0; b < 2; b++)
			{
				stack[depth].at = at + hcb_sf[at][b];
				stack[depth].c = c << 1 | b;
				stack[depth++].len = len + 1;
			}
	}
	write_out ("sf", 0, 0, 0);
	return 0;
}

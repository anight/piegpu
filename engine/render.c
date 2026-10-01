/*
 * render.c - see render.h
 */
#include "render.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "world_program.h"
#include "liquid_program.h"
#include "skydome_program.h"

enum { KIND_LIT, KIND_BRIGHT, KIND_WARP, KIND_SKY };	/* lightmapped; its own colour; a liquid; the sky */
static const int program_of[] = {0, 0, 1, 2};

typedef struct
{
	float x, y, z, u, v, lu, lv;
} vertex_t;

/* ---- textures -------------------------------------------------------------------- */

static bool power_of_two (uint32_t n)	{ return n && !(n & (n - 1)); }

static int kind_of (const char *name)
{
	return   name[0] == '*' ? KIND_WARP : strncmp (name, "sky", 3) == 0 ? KIND_SKY
	       : strncmp (name, "light", 5) == 0 ? KIND_BRIGHT : KIND_LIT;
}

#define ROWS_BYTES	2048		/* a texture's rows converted at a time */

/* a texture from 8-bit texels (w x h of them at src, stride bytes a row):
   RGB565, or RGBA5551 see-through where colour 0; a few rows at a time */
static GLuint upload (const uint8_t *src, uint32_t stride, uint32_t w, uint32_t h, bool see_through)
{
	GLenum format = see_through ? GL_RGBA : GL_RGB, type = see_through ? GL_UNSIGNED_SHORT_5_5_5_1 : GL_UNSIGNED_SHORT_5_6_5;
	uint32_t rows = ROWS_BYTES / (w * 2);
	rows = rows < 1 ? 1 : rows > h ? h : rows;
	uint16_t *texels = malloc (w * rows * 2);
	if (!texels)
	{
		return 0;
	}
	GLuint t;
	glGenTextures (1, &t);
	glBindTexture (GL_TEXTURE_2D, t);
	glPixelStorei (GL_UNPACK_ALIGNMENT, 2);
	glTexImage2D (GL_TEXTURE_2D, 0, format, w, h, 0, format, type, NULL);
	for (uint32_t y0 = 0; y0 < h; y0 += rows)
	{
		uint32_t n = h - y0 < rows ? h - y0 : rows;
		for (uint32_t y = 0; y < n; y++)
			for (uint32_t x = 0; x < w; x++)
			{
				uint8_t p = src[(y0 + y) * stride + x];
				const uint8_t *c = bsp_palette[p];
				texels[y * w + x] = see_through ? (uint16_t) ((c[0] >> 3) << 11 | (c[1] >> 3) << 6 | (c[2] >> 3) << 1 | (p != 0))
								: (uint16_t) ((c[0] >> 3) << 11 | (c[1] >> 2) << 5 | c[2] >> 3);
			}
		glTexSubImage2D (GL_TEXTURE_2D, 0, 0, y0, w, n, format, type, texels);
	}
	free (texels);
	glGenerateMipmap (GL_TEXTURE_2D);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	return t;
}

static bool upload_textures (render_t *r)
{
	const bsp_t *b = r->bsp;
	r->textures = calloc (b->n_miptex ? b->n_miptex : 1, sizeof (GLuint));
	r->fronts = calloc (b->n_miptex ? b->n_miptex : 1, sizeof (GLuint));
	r->hazes = calloc (b->n_miptex ? b->n_miptex : 1, sizeof r->hazes[0]);
	if (!r->textures || !r->fronts || !r->hazes)
	{
		return false;
	}
	for (int i = 0; i < b->n_miptex; i++)
	{
		const bsp_miptex_t *mt = bsp_miptex (b, i);
		if (!mt || !mt->offsets[0])
		{
			continue;
		}
		if (!power_of_two (mt->width) || !power_of_two (mt->height) || mt->width > 512 || mt->height > 512)
		{
			printf ("render: texture %.16s is %ux%u: not drawn (powers of two up to 512 only)\n", mt->name,
				(unsigned) mt->width, (unsigned) mt->height);
			continue;
		}
		const uint8_t *src = (const uint8_t *) mt + mt->offsets[0];
		if (kind_of (mt->name) == KIND_SKY && mt->width == 2 * mt->height)
		{
			/* the sky's two layers: the back (the right half), the front
			   (the left) see-through where colour 0; the haze the back's average */
			uint32_t h = mt->height;
			for (uint32_t y = 0; y < h; y++)
				for (uint32_t x = 0; x < h; x++)
					for (int c = 0; c < 3; c++)
						r->hazes[i][c] += bsp_palette[src[y * mt->width + h + x]][c] / (255.0f * h * h);
			r->textures[i] = upload (src + h, mt->width, h, h, false);
			r->fronts[i] = upload (src, mt->width, h, h, true);
		}
		else
		{
			r->textures[i] = upload (src, mt->width, mt->width, mt->height, false);
		}
		if (!r->textures[i])
		{
			return false;
		}
	}
	return true;
}

/* ---- lightmaps: packed into pages, rows of blocks, a block sent at a time ------------------- */

typedef struct
{
	uint8_t page, x, y;			/* the block (its padding's corner) */
	bool lit;
	int16_t bmin[2];			/* the face's texture extents' minimum, in luxels (16 texels) */
} face_light_t;

#define MAX_LUXELS	64		/* a face's lightmap across, at most */

static int shelf_x, shelf_y, shelf_h;

/* a page, its texels to come */
static bool page_new (render_t *r)
{
	if (r->n_pages >= RENDER_MAX_PAGES)
	{
		printf ("render: the lightmaps need more than %d pages\n", RENDER_MAX_PAGES);
		return false;
	}
	glGenTextures (1, &r->pages[r->n_pages]);
	glBindTexture (GL_TEXTURE_2D, r->pages[r->n_pages]);
	glTexImage2D (GL_TEXTURE_2D, 0, GL_LUMINANCE, RENDER_PAGE, RENDER_PAGE, 0, GL_LUMINANCE, GL_UNSIGNED_BYTE, NULL);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	r->n_pages++;
	shelf_x = shelf_y = shelf_h = 0;
	return true;
}

/* a block of w x h texels into the pages (a new one if needed): where; false: no pages left */
static bool page_block (render_t *r, int w, int h, const uint8_t *texels, uint8_t *page, uint8_t *x, uint8_t *y)
{
	if (shelf_x + w > RENDER_PAGE)
	{
		shelf_x = 0;
		shelf_y += shelf_h;
		shelf_h = 0;
	}
	if ((shelf_y + h > RENDER_PAGE || r->n_pages == 0) && !page_new (r))
	{
		return false;
	}
	*page = (uint8_t) (r->n_pages - 1);
	*x = (uint8_t) shelf_x;
	*y = (uint8_t) shelf_y;
	glBindTexture (GL_TEXTURE_2D, r->pages[*page]);
	glPixelStorei (GL_UNPACK_ALIGNMENT, 1);
	glTexSubImage2D (GL_TEXTURE_2D, 0, shelf_x, shelf_y, w, h, GL_LUMINANCE, GL_UNSIGNED_BYTE, texels);
	shelf_x += w;
	shelf_h = h > shelf_h ? h : shelf_h;
	return true;
}

static bool pack_lightmaps (render_t *r, face_light_t *fl, int *white_x, int *white_y)
{
	const bsp_t *b = r->bsp;
	uint8_t *block = malloc ((MAX_LUXELS + 2) * (MAX_LUXELS + 2)), page, x, y;
	if (!block)
	{
		return false;
	}
	memset (block, 128, 9);			/* the unlit faces' texel: 128, twice: their colour */
	if (!page_block (r, 3, 3, block, &page, &x, &y))
	{
		free (block);
		return false;
	}
	*white_x = x + 1;
	*white_y = y + 1;

	for (int i = 0; i < b->n_faces; i++)
	{
		const bsp_face_t *f = &b->faces[i];
		const bsp_texinfo_t *ti = &b->texinfo[f->texinfo];
		const bsp_miptex_t *mt = bsp_miptex (b, ti->miptex);
		fl[i].lit = f->light_ofs >= 0 && f->styles[0] != 255 && mt && kind_of (mt->name) == KIND_LIT;
		fl[i].page = 0;
		if (!fl[i].lit)
		{
			continue;
		}
		float mn[2] = {1e30f, 1e30f}, mx[2] = {-1e30f, -1e30f};
		for (int k = 0; k < f->edges; k++)
		{
			const float *v = bsp_face_vertex (b, f, k);
			for (int a = 0; a < 2; a++)
			{
				float s = v[0] * ti->vecs[a][0] + v[1] * ti->vecs[a][1] + v[2] * ti->vecs[a][2] + ti->vecs[a][3];
				mn[a] = s < mn[a] ? s : mn[a];
				mx[a] = s > mx[a] ? s : mx[a];
			}
		}
		for (int a = 0; a < 2; a++)
		{
			fl[i].bmin[a] = (int16_t) floorf (mn[a] / 16);
		}
		int w = (int) ceilf (mx[0] / 16) - fl[i].bmin[0] + 1, h = (int) ceilf (mx[1] / 16) - fl[i].bmin[1] + 1;
		if (w > MAX_LUXELS || h > MAX_LUXELS || (size_t) f->light_ofs + w * h > b->lighting_size)
		{
			fl[i].lit = false;		/* (broken: shown unlit) */
			continue;
		}
		const uint8_t *src = b->lighting + f->light_ofs;
		for (int ty = -1; ty <= h; ty++)		/* with its edges copied round it */
			for (int tx = -1; tx <= w; tx++)
			{
				int sx = tx < 0 ? 0 : tx >= w ? w - 1 : tx;
				int sy = ty < 0 ? 0 : ty >= h ? h - 1 : ty;
				block[(ty + 1) * (w + 2) + tx + 1] = src[sy * w + sx];
			}
		if (!page_block (r, w + 2, h + 2, block, &fl[i].page, &fl[i].x, &fl[i].y))
		{
			free (block);
			return false;
		}
	}
	free (block);
	return true;
}

/* ---- the faces' triangles, batch by batch ------------------------------------------------------ */

static int face_model (const bsp_t *b, int face)
{
	for (int m = 0; m < b->n_models; m++)
	{
		if (face >= b->models[m].first_face && face < b->models[m].first_face + b->models[m].faces)
		{
			return m;
		}
	}
	return -1;
}

static int32_t *face_keys;			/* model, texture (the sky's last), page: packed */

static int by_key (const void *a, const void *b)
{
	int ia = *(const uint16_t *) a, ib = *(const uint16_t *) b;
	return face_keys[ia] < face_keys[ib] ? -1 : face_keys[ia] > face_keys[ib] ? 1 : ia - ib;
}

#define CHUNK		64		/* vertices sent at a time */

bool render_init (render_t *r, const bsp_t *b)
{
	memset (r, 0, sizeof *r);
	r->bsp = b;
	r->pvs_leaf = -1;

	/* kept: a face's first vertex, a bit a face, the order, the PVS */
	r->first = malloc (b->n_faces * sizeof *r->first);
	r->shown = calloc ((b->n_faces + 7) / 8, 1);
	r->order = malloc (b->n_faces * sizeof *r->order);
	r->pvs = malloc ((b->n_leaves + 7) / 8);
	r->model_batches = calloc (b->n_models + 1, sizeof *r->model_batches);
	/* for now: the faces' lightmap blocks and sort keys, the vertices on their way */
	face_light_t *fl = calloc (b->n_faces, sizeof *fl);
	face_keys = malloc (b->n_faces * sizeof *face_keys);
	vertex_t *chunk = malloc (CHUNK * sizeof *chunk);
	int white_x, white_y;
	bool ok =    r->first && r->shown && r->order && r->pvs && r->model_batches && fl && face_keys && chunk
		  && upload_textures (r) && pack_lightmaps (r, fl, &white_x, &white_y);

	/* the faces in batch order: by model, texture (the sky's last), lightmap page */
	int n = 0, vertices = 0;
	for (int i = 0; ok && i < b->n_faces; i++)
	{
		int m = face_model (b, i);
		if (m < 0 || b->faces[i].edges < 3)
		{
			continue;
		}
		int t = b->texinfo[b->faces[i].texinfo].miptex;
		const bsp_miptex_t *mt = bsp_miptex (b, t);
		bool sky = mt && kind_of (mt->name) == KIND_SKY;
		face_keys[i] = (m << 20) | (sky << 19) | (t & 0x7FF) << 8 | fl[i].page;
		r->order[n++] = (uint16_t) i;
		vertices += (b->faces[i].edges - 2) * 3;
	}
	if (ok && vertices > 65535)
	{
		printf ("render: %d vertices, more than 16-bit indices reach\n", vertices);
		ok = false;
	}
	if (ok)
	{
		qsort (r->order, n, sizeof *r->order, by_key);
		r->n_batches = 0;
		for (int k = 0; k < n; k++)
		{
			r->n_batches += k == 0 || face_keys[r->order[k]] != face_keys[r->order[k - 1]];
		}
		r->batches = malloc ((r->n_batches ? r->n_batches : 1) * sizeof *r->batches);
		ok = r->batches != NULL;
	}
	for (int k = 0, nb = 0; ok && k < n; k++)
	{
		int i = r->order[k];
		if (k == 0 || face_keys[i] != face_keys[r->order[k - 1]])
		{
			render_batch_t *bt = &r->batches[nb++];
			bt->model = (uint16_t) (face_keys[i] >> 20);
			bt->texture = (uint16_t) b->texinfo[b->faces[i].texinfo].miptex;
			bt->page = fl[i].page;
			const bsp_miptex_t *mt = bsp_miptex (b, bt->texture);
			bt->kind = (uint8_t) (mt ? kind_of (mt->name) : KIND_LIT);
			bt->first_face = (uint16_t) k;
			bt->faces = 0;
		}
		r->batches[nb - 1].faces++;
	}
	for (int m = 0, k = 0; ok && m <= b->n_models; m++)
	{
		while (k < r->n_batches && r->batches[k].model < m)
		{
			k++;
		}
		r->model_batches[m] = (uint16_t) k;
	}

	/* the vertices: a fan a face, sent a chunk at a time */
	if (ok)
	{
		glGenBuffers (1, &r->buffer);
		glBindBuffer (GL_ARRAY_BUFFER, r->buffer);
		glBufferData (GL_ARRAY_BUFFER, vertices * (GLsizeiptr) sizeof (vertex_t), NULL, GL_STATIC_DRAW);
	}
	int in_chunk = 0, written = 0, at = 0;
	for (int k = 0; ok && k < n; k++)
	{
		int i = r->order[k];
		const bsp_face_t *f = &b->faces[i];
		const bsp_texinfo_t *ti = &b->texinfo[f->texinfo];
		const bsp_miptex_t *mt = bsp_miptex (b, ti->miptex);
		float tw = mt ? (float) mt->width : 64.0f, th = mt ? (float) mt->height : 64.0f;
		r->first[i] = (uint16_t) at;
		at += (f->edges - 2) * 3;
		for (int t = 1; t + 1 < f->edges; t++)
		{
			const int corner[3] = {0, t, t + 1};
			for (int c = 0; c < 3; c++)
			{
				const float *p = bsp_face_vertex (b, f, corner[c]);
				float s = p[0] * ti->vecs[0][0] + p[1] * ti->vecs[0][1] + p[2] * ti->vecs[0][2] + ti->vecs[0][3];
				float u = p[0] * ti->vecs[1][0] + p[1] * ti->vecs[1][1] + p[2] * ti->vecs[1][2] + ti->vecs[1][3];
				vertex_t *v = &chunk[in_chunk++];
				v->x = p[0];
				v->y = p[1];
				v->z = p[2];
				v->u = s / tw;
				v->v = u / th;
				if (fl[i].lit)
				{
					v->lu = (fl[i].x + 1 + (s - fl[i].bmin[0] * 16 + 8) / 16) / RENDER_PAGE;
					v->lv = (fl[i].y + 1 + (u - fl[i].bmin[1] * 16 + 8) / 16) / RENDER_PAGE;
				}
				else
				{
					v->lu = (white_x + 0.5f) / RENDER_PAGE;
					v->lv = (white_y + 0.5f) / RENDER_PAGE;
				}
				if (in_chunk == CHUNK)
				{
					glBufferSubData (GL_ARRAY_BUFFER, written * (GLintptr) sizeof (vertex_t), in_chunk * sizeof (vertex_t), chunk);
					written += in_chunk;
					in_chunk = 0;
				}
			}
		}
	}
	if (ok && in_chunk)
	{
		glBufferSubData (GL_ARRAY_BUFFER, written * (GLintptr) sizeof (vertex_t), in_chunk * sizeof (vertex_t), chunk);
	}
	free (chunk);
	free (fl);
	free (face_keys);
	face_keys = NULL;
	if (!ok)
	{
		return false;
	}

	/* the programs */
	const struct { const void *info; size_t size; } binaries[3] =
	{
		{&world_info, sizeof world_info}, {&liquid_info, sizeof liquid_info}, {&skydome_info, sizeof skydome_info},
	};
	for (int k = 0; k < 3; k++)
	{
		render_program_t *p = &r->programs[k];
		p->id = glCreateProgram ();
		glProgramBinaryOES (p->id, PGL_PROGRAM_BINARY_PGPU, binaries[k].info, binaries[k].size);
		p->u_vp = glGetUniformLocation (p->id, "u_vp");
		p->u_offset = glGetUniformLocation (p->id, "u_offset");
		p->u_eye = glGetUniformLocation (p->id, "u_eye");
		p->u_time = glGetUniformLocation (p->id, "u_time");
		p->u_size = glGetUniformLocation (p->id, "u_size");
		p->u_haze = glGetUniformLocation (p->id, "u_haze");
		p->a_pos = glGetAttribLocation (p->id, "a_pos");
		p->a_uv = glGetAttribLocation (p->id, "a_uv");
		p->a_luv = glGetAttribLocation (p->id, "a_luv");
		glUseProgram (p->id);
		glUniform1i (glGetUniformLocation (p->id, k == 2 ? "u_back" : "u_texture"), 0);
		glUniform1i (glGetUniformLocation (p->id, k == 2 ? "u_front" : "u_lightmap"), 1);
	}
	printf ("render: %d faces in %d batches, %d vertices, %d textures, %d lightmap pages\n", n, r->n_batches,
		vertices, b->n_miptex, r->n_pages);
	return true;
}

/* ---- a frame --------------------------------------------------------------------------------------- */

/* a program for the batches to come (and the attributes it takes) */
static void begin (render_t *r, int program, const float vp[16], const float offset[3])
{
	const render_program_t *p = &r->programs[program];
	glUseProgram (p->id);
	glUniformMatrix4fv (p->u_vp, 1, GL_FALSE, vp);
	glUniform3f (p->u_offset, offset[0], offset[1], offset[2]);
	if (p->u_eye >= 0)
		glUniform3f (p->u_eye, r->eye[0], r->eye[1], r->eye[2]);
	if (p->u_time >= 0)
		glUniform1f (p->u_time, program == 2 ? fmodf (r->time, 16.0f) : fmodf (r->time, 6.2831853f * 64));
	glBindBuffer (GL_ARRAY_BUFFER, r->buffer);
	for (GLint i = 0; i < 4; i++)
	{
		if (i == p->a_pos || i == p->a_uv || i == p->a_luv)
			glEnableVertexAttribArray (i);
		else
			glDisableVertexAttribArray (i);
	}
	glVertexAttribPointer (p->a_pos, 3, GL_FLOAT, GL_FALSE, sizeof (vertex_t), (void *) 0);
	if (p->a_uv >= 0)
		glVertexAttribPointer (p->a_uv, 2, GL_FLOAT, GL_FALSE, sizeof (vertex_t), (void *) 12);
	if (p->a_luv >= 0)
		glVertexAttribPointer (p->a_luv, 2, GL_FLOAT, GL_FALSE, sizeof (vertex_t), (void *) 20);
}

/* a batch's textures: the texture and the lightmap page, or a liquid's
   texture and its size, or the sky's layers */
static void bind (render_t *r, const render_batch_t *bt)
{
	const bsp_t *b = r->bsp;
	bool known = bt->texture < b->n_miptex;
	glActiveTexture (GL_TEXTURE1);
	GLuint front = known && r->fronts[bt->texture] ? r->fronts[bt->texture] : known ? r->textures[bt->texture] : 0;
	glBindTexture (GL_TEXTURE_2D, bt->kind == KIND_SKY ? front : r->pages[bt->page]);
	glActiveTexture (GL_TEXTURE0);
	glBindTexture (GL_TEXTURE_2D, known ? r->textures[bt->texture] : 0);
	if (bt->kind == KIND_SKY && known)
	{
		glUniform3f (r->programs[2].u_haze, r->hazes[bt->texture][0], r->hazes[bt->texture][1], r->hazes[bt->texture][2]);
	}
	if (bt->kind == KIND_WARP)
	{
		const bsp_miptex_t *mt = known ? bsp_miptex (b, bt->texture) : NULL;
		glUniform2f (r->programs[1].u_size, mt ? (float) mt->width : 64.0f, mt ? (float) mt->height : 64.0f);
	}
}

/* the batches [first, last): their faces marked in this frame (or all), in runs */
static void draw_batches (render_t *r, int first, int last, bool all, const float vp[16], const float offset[3])
{
	const bsp_t *b = r->bsp;
	int program = -1;
	for (int k = first; k < last; k++)
	{
		const render_batch_t *bt = &r->batches[k];
		int run_first = -1, run_count = 0;
		bool bound = false;
		for (int j = bt->first_face; j <= bt->first_face + bt->faces; j++)
		{
			int i = j < bt->first_face + bt->faces ? r->order[j] : -1;
			bool show = i >= 0 && (all || (r->shown[i >> 3] & (1 << (i & 7))));
			int first = show ? r->first[i] : -1, count = show ? (b->faces[i].edges - 2) * 3 : 0;
			if (show && run_count && first == run_first + run_count)
			{
				run_count += count;		/* (the run goes on) */
				continue;
			}
			if (run_count)
			{
				if (!bound)
				{
					if (program != program_of[bt->kind])
					{
						program = program_of[bt->kind];
						begin (r, program, vp, offset);
					}
					bind (r, bt);
					bound = true;
				}
				glDrawArrays (GL_TRIANGLES, run_first, run_count);
				r->stats.draws++;
			}
			run_first = first;
			run_count = count;
			r->stats.faces += show;
		}
	}
}

void render_world (render_t *r, const float eye[3], const float vp[16])
{
	const bsp_t *b = r->bsp;
	memset (r->shown, 0, (b->n_faces + 7) / 8);
	memset (&r->stats, 0, sizeof r->stats);
	memcpy (r->eye, eye, sizeof r->eye);

	int leaf = bsp_point_leaf (b, eye);
	if (leaf != r->pvs_leaf)
	{
		bsp_leaf_pvs (b, leaf, r->pvs);
		r->pvs_leaf = leaf;
	}

	/* the frustum's planes (a x + b y + c z + d >= 0 inside) from vp's rows */
	float planes[5][4];
	for (int p = 0; p < 5; p++)
	{
		int row = p / 2, sign = p % 2 ? -1 : 1;	/* left, right, bottom, top, near */
		for (int c = 0; c < 4; c++)
		{
			planes[p][c] = vp[c * 4 + 3] + sign * vp[c * 4 + row];
		}
	}

	int visleafs = b->models[0].visleafs;
	for (int l = 1; l <= visleafs && l < b->n_leaves; l++)
	{
		if (leaf > 0 && !(r->pvs[(l - 1) >> 3] & (1 << ((l - 1) & 7))))
		{
			continue;
		}
		const bsp_leaf_t *lf = &b->leaves[l];
		bool inside = true;
		for (int p = 0; p < 5 && inside; p++)
		{
			float x = planes[p][0] >= 0 ? lf->maxs[0] : lf->mins[0];
			float y = planes[p][1] >= 0 ? lf->maxs[1] : lf->mins[1];
			float z = planes[p][2] >= 0 ? lf->maxs[2] : lf->mins[2];
			inside = planes[p][0] * x + planes[p][1] * y + planes[p][2] * z + planes[p][3] >= 0;
		}
		if (!inside)
		{
			continue;
		}
		r->stats.leaves++;
		for (int m = 0; m < lf->marks; m++)
		{
			int i = b->marks[lf->first_mark + m];
			const bsp_face_t *f = &b->faces[i];
			const bsp_plane_t *pl = &b->planes[f->plane];
			float d = pl->normal[0] * eye[0] + pl->normal[1] * eye[1] + pl->normal[2] * eye[2] - pl->dist;
			if ((f->side ? -d : d) > 0.01f)		/* facing the eye */
			{
				r->shown[i >> 3] |= (uint8_t) (1 << (i & 7));
			}
		}
	}

	static const float none[3] = {0, 0, 0};
	draw_batches (r, r->model_batches[0], r->model_batches[1], false, vp, none);
}

void render_model (render_t *r, int model, const float offset[3], const float vp[16])
{
	if (model <= 0 || model >= r->bsp->n_models)
	{
		return;
	}
	draw_batches (r, r->model_batches[model], r->model_batches[model + 1], true, vp, offset);
}

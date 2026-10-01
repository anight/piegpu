/*
 * render.c - see render.h
 */
#include "render.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "world_program.h"

enum { KIND_LIT, KIND_BRIGHT };			/* lightmapped; its own colour */

typedef struct
{
	float x, y, z, u, v, lu, lv;
} vertex_t;

/* ---- textures -------------------------------------------------------------------- */

static bool power_of_two (uint32_t n)	{ return n && !(n & (n - 1)); }

static int kind_of (const char *name)
{
	return   strncmp (name, "light", 5) == 0 || strncmp (name, "sky", 3) == 0 || name[0] == '*'
	       ? KIND_BRIGHT : KIND_LIT;
}

static bool upload_textures (render_t *r)
{
	const bsp_t *b = r->bsp;
	r->textures = calloc (b->n_miptex ? b->n_miptex : 1, sizeof (GLuint));
	if (!r->textures)
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
		uint16_t *texels = malloc (mt->width * mt->height * 2);
		if (!texels)
		{
			return false;
		}
		for (uint32_t k = 0; k < mt->width * mt->height; k++)
		{
			const uint8_t *c = bsp_palette[src[k]];
			texels[k] = (uint16_t) ((c[0] >> 3) << 11 | (c[1] >> 2) << 5 | c[2] >> 3);
		}
		glGenTextures (1, &r->textures[i]);
		glBindTexture (GL_TEXTURE_2D, r->textures[i]);
		glPixelStorei (GL_UNPACK_ALIGNMENT, 2);
		glTexImage2D (GL_TEXTURE_2D, 0, GL_RGB, mt->width, mt->height, 0, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, texels);
		glGenerateMipmap (GL_TEXTURE_2D);
		glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
		glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		free (texels);
	}
	return true;
}

/* ---- lightmaps: packed into pages, rows of blocks ---------------------------------------- */

typedef struct
{
	int page, x, y;				/* the block (its padding's corner) */
	int bmin[2];				/* the face's texture extents' minimum, in luxels (16 texels) */
	int w, h;				/* luxels */
	bool lit;
} face_light_t;

static uint8_t page_texels[RENDER_PAGE * RENDER_PAGE];
static int shelf_x, shelf_y, shelf_h;

static void page_upload (render_t *r)
{
	glGenTextures (1, &r->pages[r->n_pages]);
	glBindTexture (GL_TEXTURE_2D, r->pages[r->n_pages]);
	glPixelStorei (GL_UNPACK_ALIGNMENT, 1);
	glTexImage2D (GL_TEXTURE_2D, 0, GL_LUMINANCE, RENDER_PAGE, RENDER_PAGE, 0, GL_LUMINANCE, GL_UNSIGNED_BYTE, page_texels);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	r->n_pages++;
}

/* a block of w x h texels: its corner in the page (a new page if needed); false: no pages left */
static bool page_block (render_t *r, int w, int h, int *page, int *x, int *y)
{
	if (shelf_x + w > RENDER_PAGE)
	{
		shelf_x = 0;
		shelf_y += shelf_h;
		shelf_h = 0;
	}
	if (shelf_y + h > RENDER_PAGE)
	{
		if (r->n_pages + 1 >= RENDER_MAX_PAGES)
		{
			return false;
		}
		page_upload (r);
		memset (page_texels, 0, sizeof page_texels);
		shelf_x = shelf_y = shelf_h = 0;
	}
	*page = r->n_pages;
	*x = shelf_x;
	*y = shelf_y;
	shelf_x += w;
	shelf_h = h > shelf_h ? h : shelf_h;
	return true;
}

static bool pack_lightmaps (render_t *r, face_light_t *fl, int *white_x, int *white_y)
{
	const bsp_t *b = r->bsp;
	memset (page_texels, 0, sizeof page_texels);
	shelf_x = shelf_y = shelf_h = 0;
	int page;
	if (!page_block (r, 3, 3, &page, white_x, white_y))	/* the unlit faces' texel: 128, twice: their colour */
	{
		return false;
	}
	for (int k = 0; k < 9; k++)
	{
		page_texels[(*white_y + k / 3) * RENDER_PAGE + *white_x + k % 3] = 128;
	}
	*white_x += 1;
	*white_y += 1;

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
			fl[i].bmin[a] = (int) floorf (mn[a] / 16);
		}
		fl[i].w = (int) ceilf (mx[0] / 16) - fl[i].bmin[0] + 1;
		fl[i].h = (int) ceilf (mx[1] / 16) - fl[i].bmin[1] + 1;
		if (fl[i].w > 64 || fl[i].h > 64 || (size_t) f->light_ofs + fl[i].w * fl[i].h > b->lighting_size)
		{
			fl[i].lit = false;		/* (broken: shown unlit) */
			continue;
		}
		int x, y;
		if (!page_block (r, fl[i].w + 2, fl[i].h + 2, &fl[i].page, &x, &y))
		{
			printf ("render: the lightmaps need more than %d pages\n", RENDER_MAX_PAGES);
			return false;
		}
		fl[i].x = x;
		fl[i].y = y;
		const uint8_t *src = b->lighting + f->light_ofs;
		for (int ty = -1; ty <= fl[i].h; ty++)		/* with its edges copied round it */
			for (int tx = -1; tx <= fl[i].w; tx++)
			{
				int sx = tx < 0 ? 0 : tx >= fl[i].w ? fl[i].w - 1 : tx;
				int sy = ty < 0 ? 0 : ty >= fl[i].h ? fl[i].h - 1 : ty;
				page_texels[(y + 1 + ty) * RENDER_PAGE + x + 1 + tx] = src[sy * fl[i].w + sx];
			}
	}
	page_upload (r);
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

static int *face_keys;				/* model, texture, page: packed */

static int by_key (const void *a, const void *b)
{
	int ka = face_keys[*(const int *) a], kb = face_keys[*(const int *) b];
	return ka < kb ? -1 : ka > kb ? 1 : *(const int *) a - *(const int *) b;
}

bool render_init (render_t *r, const bsp_t *b)
{
	memset (r, 0, sizeof *r);
	r->bsp = b;
	r->pvs_leaf = -1;
	face_light_t *fl = calloc (b->n_faces, sizeof *fl);
	r->faces = calloc (b->n_faces, sizeof *r->faces);
	r->order = malloc (b->n_faces * sizeof *r->order);
	face_keys = malloc (b->n_faces * sizeof *face_keys);
	r->pvs = malloc ((b->n_leaves + 7) / 8);
	r->model_batches = calloc (b->n_models + 1, sizeof *r->model_batches);
	int white_x, white_y;
	if (   !fl || !r->faces || !r->order || !face_keys || !r->pvs || !r->model_batches
	    || !upload_textures (r) || !pack_lightmaps (r, fl, &white_x, &white_y))
	{
		free (fl);
		free (face_keys);
		return false;
	}

	/* the faces in batch order: by model, texture, lightmap page */
	int n = 0, vertices = 0;
	for (int i = 0; i < b->n_faces; i++)
	{
		int m = face_model (b, i);
		if (m < 0 || b->faces[i].edges < 3)
		{
			continue;
		}
		face_keys[i] = (m << 20) | (b->texinfo[b->faces[i].texinfo].miptex & 0xFFF) << 8 | fl[i].page;
		r->order[n++] = i;
		vertices += (b->faces[i].edges - 2) * 3;
	}
	qsort (r->order, n, sizeof *r->order, by_key);
	r->batches = malloc ((n + 1) * sizeof *r->batches);
	if (!r->batches)
	{
		return false;
	}
	for (int k = 0; k < n; k++)
	{
		int i = r->order[k];
		if (k == 0 || face_keys[i] != face_keys[r->order[k - 1]])
		{
			render_batch_t *bt = &r->batches[r->n_batches++];
			bt->model = face_keys[i] >> 20;
			bt->texture = b->texinfo[b->faces[i].texinfo].miptex;
			bt->page = fl[i].page;
			bt->first_face = k;
			bt->faces = 0;
		}
		r->batches[r->n_batches - 1].faces++;
	}
	for (int m = 0, k = 0; m <= b->n_models; m++)
	{
		while (k < r->n_batches && r->batches[k].model < m)
		{
			k++;
		}
		r->model_batches[m] = k;
	}

	/* the vertices: a fan a face, uploaded a chunk at a time */
	glGenBuffers (1, &r->buffer);
	glBindBuffer (GL_ARRAY_BUFFER, r->buffer);
	glBufferData (GL_ARRAY_BUFFER, vertices * (GLsizeiptr) sizeof (vertex_t), NULL, GL_STATIC_DRAW);
	static vertex_t chunk[192];
	int in_chunk = 0, written = 0, at = 0;
	for (int k = 0; k < n; k++)
	{
		int i = r->order[k];
		const bsp_face_t *f = &b->faces[i];
		const bsp_texinfo_t *ti = &b->texinfo[f->texinfo];
		const bsp_miptex_t *mt = bsp_miptex (b, ti->miptex);
		float tw = mt ? (float) mt->width : 64.0f, th = mt ? (float) mt->height : 64.0f;
		r->faces[i].first = at;
		r->faces[i].count = (f->edges - 2) * 3;
		at += r->faces[i].count;
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
				if (in_chunk == (int) (sizeof chunk / sizeof chunk[0]))
				{
					glBufferSubData (GL_ARRAY_BUFFER, written * (GLintptr) sizeof (vertex_t), in_chunk * sizeof (vertex_t), chunk);
					written += in_chunk;
					in_chunk = 0;
				}
			}
		}
	}
	if (in_chunk)
	{
		glBufferSubData (GL_ARRAY_BUFFER, written * (GLintptr) sizeof (vertex_t), in_chunk * sizeof (vertex_t), chunk);
	}
	free (fl);
	free (face_keys);
	face_keys = NULL;

	/* the program */
	r->program = glCreateProgram ();
	glProgramBinaryOES (r->program, PGL_PROGRAM_BINARY_PGPU, &world_info, sizeof world_info);
	r->u_vp = glGetUniformLocation (r->program, "u_vp");
	r->u_offset = glGetUniformLocation (r->program, "u_offset");
	r->a_pos = glGetAttribLocation (r->program, "a_pos");
	r->a_uv = glGetAttribLocation (r->program, "a_uv");
	r->a_luv = glGetAttribLocation (r->program, "a_luv");
	glUseProgram (r->program);
	glUniform1i (glGetUniformLocation (r->program, "u_texture"), 0);
	glUniform1i (glGetUniformLocation (r->program, "u_lightmap"), 1);
	printf ("render: %d faces in %d batches, %d vertices, %d textures, %d lightmap pages\n", n, r->n_batches,
		vertices, b->n_miptex, r->n_pages);
	return true;
}

/* ---- a frame --------------------------------------------------------------------------------------- */

static void begin (render_t *r, const float vp[16], const float offset[3])
{
	glUseProgram (r->program);
	glUniformMatrix4fv (r->u_vp, 1, GL_FALSE, vp);
	glUniform3f (r->u_offset, offset[0], offset[1], offset[2]);
	glBindBuffer (GL_ARRAY_BUFFER, r->buffer);
	for (GLint i = 0; i < 4; i++)
	{
		if (i == r->a_pos || i == r->a_uv || i == r->a_luv)
			glEnableVertexAttribArray (i);
		else
			glDisableVertexAttribArray (i);
	}
	glVertexAttribPointer (r->a_pos, 3, GL_FLOAT, GL_FALSE, sizeof (vertex_t), (void *) 0);
	glVertexAttribPointer (r->a_uv, 2, GL_FLOAT, GL_FALSE, sizeof (vertex_t), (void *) 12);
	glVertexAttribPointer (r->a_luv, 2, GL_FLOAT, GL_FALSE, sizeof (vertex_t), (void *) 20);
}

/* the batches [first, last): their faces marked in this frame (or all), in runs */
static void draw_batches (render_t *r, int first, int last, bool all)
{
	const bsp_t *b = r->bsp;
	for (int k = first; k < last; k++)
	{
		const render_batch_t *bt = &r->batches[k];
		int run_first = -1, run_count = 0;
		bool bound = false;
		for (int j = bt->first_face; j <= bt->first_face + bt->faces; j++)
		{
			const render_face_t *f = j < bt->first_face + bt->faces ? &r->faces[r->order[j]] : NULL;
			bool show = f && (all || f->frame == r->frame);
			if (show && run_count && f->first == run_first + run_count)
			{
				run_count += f->count;		/* (the run goes on) */
				continue;
			}
			if (run_count)
			{
				if (!bound)
				{
					glActiveTexture (GL_TEXTURE1);
					glBindTexture (GL_TEXTURE_2D, r->pages[bt->page]);
					glActiveTexture (GL_TEXTURE0);
					glBindTexture (GL_TEXTURE_2D, bt->texture >= 0 && bt->texture < b->n_miptex ? r->textures[bt->texture] : 0);
					bound = true;
				}
				glDrawArrays (GL_TRIANGLES, run_first, run_count);
				r->stats.draws++;
			}
			run_first = show ? f->first : -1;
			run_count = show ? f->count : 0;
			r->stats.faces += show;
		}
	}
}

void render_world (render_t *r, const float eye[3], const float vp[16])
{
	const bsp_t *b = r->bsp;
	r->frame++;
	memset (&r->stats, 0, sizeof r->stats);

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
				r->faces[i].frame = r->frame;
			}
		}
	}

	static const float none[3] = {0, 0, 0};
	begin (r, vp, none);
	draw_batches (r, r->model_batches[0], r->model_batches[1], false);
}

void render_model (render_t *r, int model, const float offset[3], const float vp[16])
{
	if (model <= 0 || model >= r->bsp->n_models)
	{
		return;
	}
	begin (r, vp, offset);
	draw_batches (r, r->model_batches[model], r->model_batches[model + 1], true);
}

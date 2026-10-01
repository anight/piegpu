/*
 * bsp.c - see bsp.h
 */
#include "bsp.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum
{
	LUMP_ENTITIES, LUMP_PLANES, LUMP_TEXTURES, LUMP_VERTICES, LUMP_VISIBILITY, LUMP_NODES,
	LUMP_TEXINFO, LUMP_FACES, LUMP_LIGHTING, LUMP_CLIPNODES, LUMP_LEAVES, LUMP_MARKS,
	LUMP_EDGES, LUMP_SURFEDGES, LUMP_MODELS, LUMPS
};

static int32_t le32 (const uint8_t *p)	{ return (int32_t) ((uint32_t) p[0] | (uint32_t) p[1] << 8 | (uint32_t) p[2] << 16 | (uint32_t) p[3] << 24); }

bool bsp_load (bsp_t *b, const void *data, size_t size)
{
	static const unsigned item[LUMPS] =	/* bytes an item: to check the sizes */
	{
		1, sizeof (bsp_plane_t), 1, 12, 1, sizeof (bsp_node_t), sizeof (bsp_texinfo_t), sizeof (bsp_face_t),
		1, sizeof (bsp_clipnode_t), sizeof (bsp_leaf_t), 2, sizeof (bsp_edge_t), 4, sizeof (bsp_model_t)
	};
	const uint8_t *d = (const uint8_t *) data;
	memset (b, 0, sizeof *b);
	if (size < 4 + LUMPS * 8 || le32 (d) != BSP_VERSION)
	{
		printf ("bsp: not a BSP29 level (version %d)\n", size >= 4 ? (int) le32 (d) : -1);
		return false;
	}
	if ((uintptr_t) d & 3)
	{
		printf ("bsp: the level's bytes aren't 4-byte aligned\n");
		return false;
	}
	const uint8_t *lump[LUMPS];
	int count[LUMPS];
	for (int i = 0; i < LUMPS; i++)
	{
		uint32_t ofs = (uint32_t) le32 (d + 4 + i * 8), len = (uint32_t) le32 (d + 8 + i * 8);
		if (ofs > size || len > size - ofs || (len % item[i]) || (ofs & 3))
		{
			printf ("bsp: lump %d is broken (offset %u, %u bytes)\n", i, (unsigned) ofs, (unsigned) len);
			return false;
		}
		lump[i] = d + ofs;
		count[i] = (int) (len / item[i]);
	}
	b->data = d;
	b->size = size;
	b->entities = (const char *) lump[LUMP_ENTITIES];
	b->entities_size = count[LUMP_ENTITIES];
	b->planes = (const bsp_plane_t *) lump[LUMP_PLANES];			b->n_planes = count[LUMP_PLANES];
	b->vertices = (const float (*)[3]) lump[LUMP_VERTICES];			b->n_vertices = count[LUMP_VERTICES];
	b->vis = lump[LUMP_VISIBILITY];						b->vis_size = count[LUMP_VISIBILITY];
	b->nodes = (const bsp_node_t *) lump[LUMP_NODES];			b->n_nodes = count[LUMP_NODES];
	b->texinfo = (const bsp_texinfo_t *) lump[LUMP_TEXINFO];		b->n_texinfo = count[LUMP_TEXINFO];
	b->faces = (const bsp_face_t *) lump[LUMP_FACES];			b->n_faces = count[LUMP_FACES];
	b->lighting = lump[LUMP_LIGHTING];					b->lighting_size = count[LUMP_LIGHTING];
	b->clipnodes = (const bsp_clipnode_t *) lump[LUMP_CLIPNODES];		b->n_clipnodes = count[LUMP_CLIPNODES];
	b->leaves = (const bsp_leaf_t *) lump[LUMP_LEAVES];			b->n_leaves = count[LUMP_LEAVES];
	b->marks = (const uint16_t *) lump[LUMP_MARKS];				b->n_marks = count[LUMP_MARKS];
	b->edges = (const bsp_edge_t *) lump[LUMP_EDGES];			b->n_edges = count[LUMP_EDGES];
	b->surfedges = (const int32_t *) lump[LUMP_SURFEDGES];			b->n_surfedges = count[LUMP_SURFEDGES];
	b->models = (const bsp_model_t *) lump[LUMP_MODELS];			b->n_models = count[LUMP_MODELS];
	if (count[LUMP_TEXTURES] >= 4)
	{
		b->miptex_lump = lump[LUMP_TEXTURES];
		b->n_miptex = le32 (b->miptex_lump);
		b->miptex_ofs = (const int32_t *) (b->miptex_lump + 4);
		if (b->n_miptex < 0 || 4 + 4 * (size_t) b->n_miptex > (size_t) count[LUMP_TEXTURES])
		{
			printf ("bsp: the texture lump is broken\n");
			return false;
		}
	}
	if (b->n_models < 1 || b->n_leaves < 1)
	{
		printf ("bsp: no world model\n");
		return false;
	}
	return true;
}

const bsp_miptex_t *bsp_miptex (const bsp_t *b, int index)
{
	if (index < 0 || index >= b->n_miptex || b->miptex_ofs[index] < 0)
	{
		return NULL;
	}
	return (const bsp_miptex_t *) (b->miptex_lump + b->miptex_ofs[index]);
}

int bsp_point_leaf (const bsp_t *b, const float p[3])
{
	int n = b->models[0].headnode[0];
	while (n >= 0)
	{
		const bsp_node_t *node = &b->nodes[n];
		const bsp_plane_t *pl = &b->planes[node->plane];
		float d = pl->normal[0] * p[0] + pl->normal[1] * p[1] + pl->normal[2] * p[2] - pl->dist;
		n = node->children[d < 0.0f];
	}
	return -1 - n;
}

void bsp_leaf_pvs (const bsp_t *b, int leaf, uint8_t *out)
{
	int bytes = (b->n_leaves + 7) / 8, visleafs = b->models[0].visleafs;
	int32_t ofs = leaf > 0 && leaf < b->n_leaves ? b->leaves[leaf].vis_ofs : -1;
	if (ofs < 0 || (size_t) ofs >= b->vis_size)
	{
		memset (out, 0xFF, bytes);		/* no vis: everything */
		return;
	}
	/* run-length: a 0 byte, then how many zero bytes */
	const uint8_t *v = b->vis + ofs, *end = b->vis + b->vis_size;
	int row = (visleafs + 7) / 8, k = 0;
	memset (out, 0, bytes);
	while (k < row && v < end)
	{
		if (*v)
		{
			out[k++] = *v++;
		}
		else if (v + 1 < end)
		{
			k += v[1];
			v += 2;
		}
		else
		{
			break;
		}
	}
}

const float *bsp_face_vertex (const bsp_t *b, const bsp_face_t *f, int k)
{
	int32_t e = b->surfedges[f->first_edge + k];
	return b->vertices[e >= 0 ? b->edges[e].v[0] : b->edges[-e].v[1]];
}

/* ---- entities ---- */

static const char *token (const char *p, const char *end, char *out, size_t max, bool *quoted)
{
	while (p < end && *p && (unsigned char) *p <= ' ')
	{
		p++;
	}
	if (p >= end || !*p)
	{
		return NULL;
	}
	size_t n = 0;
	*quoted = *p == '"';
	if (*quoted)
	{
		p++;
		while (p < end && *p && *p != '"')
		{
			if (n + 1 < max)
			{
				out[n++] = *p;
			}
			p++;
		}
		p += p < end && *p == '"';
	}
	else
	{
		out[n++] = *p++;			/* a brace */
	}
	out[n] = '\0';
	return p;
}

bool bsp_entity_next (const bsp_t *b, const char **cursor, bsp_entity_t *e)
{
	const char *p = *cursor ? *cursor : b->entities, *end = b->entities + b->entities_size;
	char t[64];
	bool q;
	memset (e, 0, sizeof *e);
	if (!(p = token (p, end, t, sizeof t, &q)) || q || t[0] != '{')
	{
		return false;
	}
	while ((p = token (p, end, t, sizeof t, &q)) != NULL)
	{
		if (!q && t[0] == '}')
		{
			*cursor = p;
			return true;
		}
		char value[64];
		if (!(p = token (p, end, value, sizeof value, &q)))
		{
			break;
		}
		if (e->pairs < BSP_MAX_PAIRS)
		{
			snprintf (e->key[e->pairs], sizeof e->key[0], "%s", t);
			snprintf (e->value[e->pairs], sizeof e->value[0], "%s", value);
			e->pairs++;
		}
	}
	return false;
}

const char *bsp_entity_value (const bsp_entity_t *e, const char *key)
{
	for (int i = 0; i < e->pairs; i++)
	{
		if (strcmp (e->key[i], key) == 0)
		{
			return e->value[i];
		}
	}
	return NULL;
}

bool bsp_entity_vector (const bsp_entity_t *e, const char *key, float v[3])
{
	const char *s = bsp_entity_value (e, key);
	return s && sscanf (s, "%f %f %f", &v[0], &v[1], &v[2]) == 3;
}

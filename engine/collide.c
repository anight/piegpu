/*
 * collide.c - see collide.h. A trace walks the hull's tree from the start to
 * the end, splitting the segment where it crosses a node's plane; the first
 * crossing from open space into solid is the hit (a little before it: the
 * point stays in the open).
 */
#include "collide.h"
#include <stdlib.h>
#include <string.h>

#define DIST_EPSILON	0.03125f	/* stop this far in front of a plane */

typedef struct
{
	const bsp_clipnode_t *nodes;
	const bsp_plane_t *planes;
	int head;
} hull_t;

bool collide_init (collide_t *c, const bsp_t *b)
{
	memset (c, 0, sizeof *c);
	c->bsp = b;
	c->hull0 = malloc (b->n_nodes * sizeof *c->hull0);
	if (!c->hull0)
	{
		return false;
	}
	for (int i = 0; i < b->n_nodes; i++)
	{
		c->hull0[i].plane = b->nodes[i].plane;
		for (int k = 0; k < 2; k++)
		{
			int child = b->nodes[i].children[k];
			c->hull0[i].children[k] = (int16_t) (child >= 0 ? child : b->leaves[-1 - child].contents);
		}
	}
	return true;
}

int collide_add_solid (collide_t *c, int model)
{
	if (c->n_solids >= COLLIDE_MAX_SOLIDS || model <= 0 || model >= c->bsp->n_models)
	{
		return -1;
	}
	collide_solid_t *s = &c->solids[c->n_solids];
	memset (s, 0, sizeof *s);
	s->model = model;
	s->active = true;
	return c->n_solids++;
}

void collide_move_solid (collide_t *c, int solid, const float offset[3])
{
	if (solid >= 0 && solid < c->n_solids)
	{
		memcpy (c->solids[solid].offset, offset, sizeof c->solids[solid].offset);
	}
}

static hull_t hull_of (const collide_t *c, int model, int hull)
{
	hull_t h = {hull == 0 ? c->hull0 : c->bsp->clipnodes, c->bsp->planes, c->bsp->models[model].headnode[hull]};
	return h;
}

static float plane_dist (const bsp_plane_t *pl, const float p[3])
{
	return pl->type < 3 ? p[pl->type] - pl->dist
			    : pl->normal[0] * p[0] + pl->normal[1] * p[1] + pl->normal[2] * p[2] - pl->dist;
}

static int point_contents (const hull_t *h, int num, const float p[3])
{
	while (num >= 0)
	{
		const bsp_clipnode_t *n = &h->nodes[num];
		num = n->children[plane_dist (&h->planes[n->plane], p) < 0.0f];
	}
	return num;
}

/* the segment p1 (at fraction f1) to p2 (f2) through node num: false once it hit */
static bool trace_node (const hull_t *h, int num, float f1, float f2, const float p1[3], const float p2[3], collide_trace_t *t)
{
	if (num < 0)					/* a leaf: its contents */
	{
		if (num != BSP_CONTENTS_SOLID)
		{
			t->all_solid = false;
		}
		else
		{
			t->start_solid = true;
		}
		return true;
	}
	const bsp_clipnode_t *n = &h->nodes[num];
	const bsp_plane_t *pl = &h->planes[n->plane];
	float d1 = plane_dist (pl, p1), d2 = plane_dist (pl, p2);
	if (d1 >= 0.0f && d2 >= 0.0f)
	{
		return trace_node (h, n->children[0], f1, f2, p1, p2, t);
	}
	if (d1 < 0.0f && d2 < 0.0f)
	{
		return trace_node (h, n->children[1], f1, f2, p1, p2, t);
	}

	/* it crosses the plane: the near side first, then the far one */
	float frac = d1 < 0.0f ? (d1 + DIST_EPSILON) / (d1 - d2) : (d1 - DIST_EPSILON) / (d1 - d2);
	frac = frac < 0.0f ? 0.0f : frac > 1.0f ? 1.0f : frac;
	float fm = f1 + (f2 - f1) * frac, mid[3];
	for (int k = 0; k < 3; k++)
	{
		mid[k] = p1[k] + frac * (p2[k] - p1[k]);
	}
	int side = d1 < 0.0f;
	if (!trace_node (h, n->children[side], f1, fm, p1, mid, t))
	{
		return false;
	}
	if (point_contents (h, n->children[side ^ 1], mid) != BSP_CONTENTS_SOLID)
	{
		return trace_node (h, n->children[side ^ 1], fm, f2, mid, p2, t);
	}
	if (t->all_solid)
	{
		return false;				/* never got out of the solid */
	}

	/* the far side is solid: the hit, on this plane */
	for (int k = 0; k < 3; k++)
	{
		t->normal[k] = side ? -pl->normal[k] : pl->normal[k];
	}
	while (point_contents (h, h->head, mid) == BSP_CONTENTS_SOLID)
	{
		frac -= 0.1f;				/* (the epsilon put it in the solid: back off) */
		if (frac < 0.0f)
		{
			t->fraction = f1;
			memcpy (t->end, p1, sizeof t->end);
			return false;
		}
		fm = f1 + (f2 - f1) * frac;
		for (int k = 0; k < 3; k++)
		{
			mid[k] = p1[k] + frac * (p2[k] - p1[k]);
		}
	}
	t->fraction = fm;
	memcpy (t->end, mid, sizeof t->end);
	return false;
}

static collide_trace_t trace_hull (const hull_t *h, const float offset[3], const float start[3], const float end[3])
{
	collide_trace_t t;
	memset (&t, 0, sizeof t);
	t.fraction = 1.0f;
	t.all_solid = true;
	float s[3], e[3];
	for (int k = 0; k < 3; k++)
	{
		s[k] = start[k] - offset[k];
		e[k] = end[k] - offset[k];
	}
	trace_node (h, h->head, 0.0f, 1.0f, s, e, &t);
	if (t.fraction == 1.0f)
	{
		memcpy (t.end, end, sizeof t.end);
	}
	else
	{
		for (int k = 0; k < 3; k++)
		{
			t.end[k] += offset[k];
		}
	}
	return t;
}

collide_trace_t collide_trace_solid (const collide_t *c, int solid, int hull, const float start[3], const float end[3])
{
	hull_t h = hull_of (c, c->solids[solid].model, hull);
	collide_trace_t t = trace_hull (&h, c->solids[solid].offset, start, end);
	t.entity = solid;
	return t;
}

collide_trace_t collide_trace (const collide_t *c, int hull, const float start[3], const float end[3], int ignore)
{
	static const float none[3] = {0, 0, 0};
	hull_t h = hull_of (c, 0, hull);
	collide_trace_t best = trace_hull (&h, none, start, end);
	best.entity = -1;
	for (int i = 0; i < c->n_solids; i++)
	{
		if (i == ignore || !c->solids[i].active)
		{
			continue;
		}
		collide_trace_t t = collide_trace_solid (c, i, hull, start, end);
		if (t.all_solid || t.start_solid || t.fraction < best.fraction)
		{
			bool was_start = best.start_solid;
			if (t.fraction < best.fraction || t.all_solid)
			{
				best = t;
			}
			best.start_solid = best.start_solid || was_start;
		}
	}
	return best;
}

int collide_contents (const collide_t *c, int hull, const float p[3])
{
	hull_t h = hull_of (c, 0, hull);
	int contents = point_contents (&h, h.head, p);
	for (int i = 0; i < c->n_solids && contents != BSP_CONTENTS_SOLID; i++)
	{
		if (!c->solids[i].active)
		{
			continue;
		}
		hull_t s = hull_of (c, c->solids[i].model, hull);
		float q[3] = {p[0] - c->solids[i].offset[0], p[1] - c->solids[i].offset[1], p[2] - c->solids[i].offset[2]};
		if (point_contents (&s, s.head, q) == BSP_CONTENTS_SOLID)
		{
			contents = BSP_CONTENTS_SOLID;
		}
	}
	return contents;
}

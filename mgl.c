/*
 * mgl.c -- implementation. See mgl.h for scope/design notes.
 */

#include "mgl.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define MGL_STACK_DEPTH 16
#define MGL_MAX_VERTS    4096

typedef struct {
	float x, y, z;          /* screen space (viewport-mapped)  */
	float depth;             /* [0,1], 0 = near                 */
	float r, g, b;
	int   clipped;           /* dropped (w <= 0)                */
} mgl_vertex_t;

struct mgl_ctx {
	uint8_t* buf;
	size_t   pitch;
	int      width, height;

	int      vx, vy, vw, vh;

	float*   depthbuf;       /* width*height, valid within vw/vh */
	int      depth_alloc_w, depth_alloc_h;

	/* internal, tightly-packed colour back buffer -- all drawing lands here;
	   mgl_swap_buffers() blits it to the real (possibly padded-stride)
	   target in one pass, so the caller's visible buffer is never seen
	   half-drawn (avoids flicker/tearing, same idea as fb.c's back buffer). */
	uint8_t* backbuf;
	size_t   back_pitch;
	int      back_alloc_w, back_alloc_h;

	mgl_matrixmode_t mode;
	mgl_mat4 mv_stack[MGL_STACK_DEPTH];
	mgl_mat4 pr_stack[MGL_STACK_DEPTH];
	int      mv_top, pr_top;

	float    clear_r, clear_g, clear_b, clear_a;
	int      depth_test, cull_face;

	/* immediate-mode accumulation */
	mgl_prim_t   prim;
	int          in_begin;
	float        cur_r, cur_g, cur_b;
	mgl_vertex_t verts[MGL_MAX_VERTS];
	int          nverts;
};

static mgl_ctx_t* g_ctx;

/* ------------------------------------------------------------------ *
 * 4x4 matrix helpers (column-major, same layout/semantics as GL).    *
 * ------------------------------------------------------------------ */
static void mat4_identity(mgl_mat4 m) {
	memset(m, 0, sizeof(mgl_mat4));
	m[0] = m[5] = m[10] = m[15] = 1.0f;
}

static void mat4_mul(mgl_mat4 r, const mgl_mat4 a, const mgl_mat4 b) {
	mgl_mat4 t;
	int col, row, k;
	for (col = 0; col < 4; col++)
		for (row = 0; row < 4; row++) {
			float s = 0.0f;
			for (k = 0; k < 4; k++)
				s += a[k * 4 + row] * b[col * 4 + k];
			t[col * 4 + row] = s;
		}
	memcpy(r, t, sizeof(mgl_mat4));
}

static void mat4_vec4(float out[4], const mgl_mat4 m, const float v[4]) {
	int row, k;
	for (row = 0; row < 4; row++) {
		float s = 0.0f;
		for (k = 0; k < 4; k++)
			s += m[k * 4 + row] * v[k];
		out[row] = s;
	}
}

/* ------------------------------------------------------------------ *
 * Context / target.                                                  *
 * ------------------------------------------------------------------ */
mgl_ctx_t* mgl_create(int width, int height) {
	mgl_ctx_t* c = calloc(1, sizeof(*c));
	if (c == NULL)
		return NULL;

	mat4_identity(c->mv_stack[0]);
	mat4_identity(c->pr_stack[0]);
	c->mv_top = c->pr_top = 0;
	c->mode = MGL_MODELVIEW;

	c->clear_r = c->clear_g = c->clear_b = 0.0f;
	c->clear_a = 1.0f;
	c->depth_test = 1;
	c->cull_face  = 0;

	if (width > 0 && height > 0) {
		c->depthbuf = malloc((size_t)width * (size_t)height * sizeof(float));
		c->depth_alloc_w = width;
		c->depth_alloc_h = height;
		c->backbuf = malloc((size_t)width * (size_t)height * 4);
		c->back_alloc_w = width;
		c->back_alloc_h = height;
		c->back_pitch = (size_t)width * 4;
	}
	c->width = width;
	c->height = height;
	c->vx = 0; c->vy = 0; c->vw = width; c->vh = height;

	return c;
}

void mgl_destroy(mgl_ctx_t* ctx) {
	if (ctx == NULL)
		return;
	free(ctx->depthbuf);
	free(ctx->backbuf);
	free(ctx);
	if (g_ctx == ctx)
		g_ctx = NULL;
}

void mgl_make_current(mgl_ctx_t* ctx) {
	g_ctx = ctx;
}

void mgl_set_target(uint8_t* buf, size_t pitch, int width, int height) {
	mgl_ctx_t* c = g_ctx;
	if (c == NULL)
		return;
	c->buf = buf;
	c->pitch = pitch;
	c->width = width;
	c->height = height;
	c->vx = 0; c->vy = 0; c->vw = width; c->vh = height;

	if (width > c->depth_alloc_w || height > c->depth_alloc_h) {
		size_t need = (size_t)width * (size_t)height * sizeof(float);
		float* nb = realloc(c->depthbuf, need);
		if (nb != NULL) {
			c->depthbuf = nb;
			c->depth_alloc_w = width;
			c->depth_alloc_h = height;
		}
	}
	if (width > c->back_alloc_w || height > c->back_alloc_h) {
		size_t need = (size_t)width * (size_t)height * 4;
		uint8_t* nb = realloc(c->backbuf, need);
		if (nb != NULL) {
			c->backbuf = nb;
			c->back_alloc_w = width;
			c->back_alloc_h = height;
		}
	}
	c->back_pitch = (size_t)width * 4;
}

void mgl_viewport(int x, int y, int w, int h) {
	mgl_ctx_t* c = g_ctx;
	if (c == NULL)
		return;
	c->vx = x; c->vy = y; c->vw = w; c->vh = h;
}

/* ------------------------------------------------------------------ *
 * State.                                                              *
 * ------------------------------------------------------------------ */
void mgl_clear_color(float r, float g, float b, float a) {
	if (g_ctx == NULL) return;
	g_ctx->clear_r = r; g_ctx->clear_g = g; g_ctx->clear_b = b; g_ctx->clear_a = a;
}

/* Clears (colour and/or depth) are scoped to the current viewport rect, not
   the whole target -- glcube.c's mgl target is the *whole screen* buffer
   (desktop + window chrome are hand-drawn into the rest of it outside the
   cube's viewport), so a full-buffer clear/swap would wipe that. */
void mgl_clear(int color, int depth) {
	mgl_ctx_t* c = g_ctx;
	int x, y, x0, y0, x1, y1;
	if (c == NULL)
		return;

	x0 = c->vx; y0 = c->vy;
	x1 = c->vx + c->vw; y1 = c->vy + c->vh;
	if (x0 < 0) x0 = 0;
	if (y0 < 0) y0 = 0;
	if (x1 > c->width)  x1 = c->width;
	if (y1 > c->height) y1 = c->height;

	if (color && c->backbuf != NULL) {
		unsigned r = (unsigned)(c->clear_r * 255.0f);
		unsigned g = (unsigned)(c->clear_g * 255.0f);
		unsigned b = (unsigned)(c->clear_b * 255.0f);
		uint32_t px = (r << 16) | (g << 8) | b;
		for (y = y0; y < y1; y++) {
			uint32_t* line = (uint32_t*)(c->backbuf + (size_t)y * c->back_pitch);
			for (x = x0; x < x1; x++)
				line[x] = px;
		}
	}
	if (depth && c->depthbuf != NULL) {
		for (y = y0; y < y1; y++) {
			float* line = c->depthbuf + (size_t)y * (size_t)c->width;
			for (x = x0; x < x1; x++)
				line[x] = 1.0f;
		}
	}
}

void mgl_enable_depth_test(int on) { if (g_ctx) g_ctx->depth_test = on; }
void mgl_enable_cull_face(int on)  { if (g_ctx) g_ctx->cull_face  = on; }

/* ------------------------------------------------------------------ *
 * Matrix stack.                                                       *
 * ------------------------------------------------------------------ */
static mgl_mat4* cur_matrix(mgl_ctx_t* c) {
	return (c->mode == MGL_MODELVIEW) ? &c->mv_stack[c->mv_top]
	                                   : &c->pr_stack[c->pr_top];
}

void mgl_matrix_mode(mgl_matrixmode_t m) { if (g_ctx) g_ctx->mode = m; }

void mgl_load_identity(void) {
	if (g_ctx == NULL) return;
	mat4_identity(*cur_matrix(g_ctx));
}

void mgl_push_matrix(void) {
	mgl_ctx_t* c = g_ctx;
	if (c == NULL) return;
	if (c->mode == MGL_MODELVIEW) {
		if (c->mv_top + 1 < MGL_STACK_DEPTH) {
			memcpy(c->mv_stack[c->mv_top + 1], c->mv_stack[c->mv_top], sizeof(mgl_mat4));
			c->mv_top++;
		}
	} else {
		if (c->pr_top + 1 < MGL_STACK_DEPTH) {
			memcpy(c->pr_stack[c->pr_top + 1], c->pr_stack[c->pr_top], sizeof(mgl_mat4));
			c->pr_top++;
		}
	}
}

void mgl_pop_matrix(void) {
	mgl_ctx_t* c = g_ctx;
	if (c == NULL) return;
	if (c->mode == MGL_MODELVIEW) { if (c->mv_top > 0) c->mv_top--; }
	else                          { if (c->pr_top > 0) c->pr_top--; }
}

void mgl_mult_matrix(const mgl_mat4 m) {
	mgl_ctx_t* c = g_ctx;
	mgl_mat4* top;
	if (c == NULL) return;
	top = cur_matrix(c);
	mat4_mul(*top, *top, m);
}

void mgl_translate(float x, float y, float z) {
	mgl_mat4 t;
	mat4_identity(t);
	t[12] = x; t[13] = y; t[14] = z;
	mgl_mult_matrix(t);
}

void mgl_scale(float x, float y, float z) {
	mgl_mat4 s;
	mat4_identity(s);
	s[0] = x; s[5] = y; s[10] = z;
	mgl_mult_matrix(s);
}

void mgl_rotate(float deg, float x, float y, float z) {
	float rad = deg * (float)M_PI / 180.0f;
	float c = cosf(rad), s = sinf(rad), ic = 1.0f - c;
	float len = sqrtf(x * x + y * y + z * z);
	mgl_mat4 r;
	if (len < 1e-9f)
		return;
	x /= len; y /= len; z /= len;

	mat4_identity(r);
	r[0] = x * x * ic + c;       r[4] = x * y * ic - z * s;   r[8]  = x * z * ic + y * s;
	r[1] = y * x * ic + z * s;   r[5] = y * y * ic + c;       r[9]  = y * z * ic - x * s;
	r[2] = x * z * ic - y * s;   r[6] = y * z * ic + x * s;   r[10] = z * z * ic + c;
	mgl_mult_matrix(r);
}

void mgl_frustum(float l, float r, float b, float t, float n, float f) {
	mgl_mat4 m;
	memset(m, 0, sizeof(m));
	m[0]  = (2.0f * n) / (r - l);
	m[5]  = (2.0f * n) / (t - b);
	m[8]  = (r + l) / (r - l);
	m[9]  = (t + b) / (t - b);
	m[10] = -(f + n) / (f - n);
	m[11] = -1.0f;
	m[14] = -(2.0f * f * n) / (f - n);
	mgl_mult_matrix(m);
}

void mgl_ortho(float l, float r, float b, float t, float n, float f) {
	mgl_mat4 m;
	mat4_identity(m);
	m[0]  = 2.0f / (r - l);
	m[5]  = 2.0f / (t - b);
	m[10] = -2.0f / (f - n);
	m[12] = -(r + l) / (r - l);
	m[13] = -(t + b) / (t - b);
	m[14] = -(f + n) / (f - n);
	mgl_mult_matrix(m);
}

void mgl_perspective(float fovy_deg, float aspect, float znear, float zfar) {
	float fovy_rad = fovy_deg * (float)M_PI / 180.0f;
	float top = znear * tanf(fovy_rad * 0.5f);
	float right = top * aspect;
	mgl_frustum(-right, right, -top, top, znear, zfar);
}

/* ------------------------------------------------------------------ *
 * Immediate mode.                                                     *
 * ------------------------------------------------------------------ */
void mgl_begin(mgl_prim_t prim) {
	mgl_ctx_t* c = g_ctx;
	if (c == NULL) return;
	c->prim = prim;
	c->in_begin = 1;
	c->nverts = 0;
	c->cur_r = c->cur_g = c->cur_b = 1.0f;
}

void mgl_color3f(float r, float g, float b) {
	if (g_ctx == NULL) return;
	g_ctx->cur_r = r; g_ctx->cur_g = g; g_ctx->cur_b = b;
}

void mgl_vertex3f(float x, float y, float z) {
	mgl_ctx_t* c = g_ctx;
	mgl_mat4 mvp;
	float obj[4], clip[4];
	mgl_vertex_t* v;

	if (c == NULL || !c->in_begin || c->nverts >= MGL_MAX_VERTS)
		return;

	mat4_mul(mvp, c->pr_stack[c->pr_top], c->mv_stack[c->mv_top]);
	obj[0] = x; obj[1] = y; obj[2] = z; obj[3] = 1.0f;
	mat4_vec4(clip, mvp, obj);

	v = &c->verts[c->nverts++];
	v->r = c->cur_r; v->g = c->cur_g; v->b = c->cur_b;

	if (clip[3] <= 1e-6f) {
		v->clipped = 1;
		return;
	}
	v->clipped = 0;
	{
		float invw = 1.0f / clip[3];
		float ndc_x = clip[0] * invw;
		float ndc_y = clip[1] * invw;
		float ndc_z = clip[2] * invw;
		v->x = (float)c->vx + (ndc_x * 0.5f + 0.5f) * (float)c->vw;
		v->y = (float)c->vy + (1.0f - (ndc_y * 0.5f + 0.5f)) * (float)c->vh;
		v->depth = ndc_z * 0.5f + 0.5f;
	}
}

/* ---- rasterizer ---- */
static void put_px_depth(mgl_ctx_t* c, int x, int y, float depth, uint32_t col) {
	size_t idx;
	if ((unsigned)x >= (unsigned)c->width || (unsigned)y >= (unsigned)c->height)
		return;
	idx = (size_t)y * (size_t)c->width + (size_t)x;
	if (c->depth_test && c->depthbuf != NULL) {
		if (depth >= c->depthbuf[idx])
			return;
		c->depthbuf[idx] = depth;
	}
	if (c->backbuf == NULL)
		return;
	*(uint32_t*)(c->backbuf + (size_t)y * c->back_pitch + (size_t)x * 4) = col;
}

/* Blit the internal back buffer to the real target buffer, scoped to the
   current viewport rect (see mgl_clear's comment above), in one pass. Call
   once per frame, after all drawing is done and before the caller presents
   the target (fb_flip() / FBVT_COMMIT) -- avoids the target ever being
   visible half-drawn. */
void mgl_swap_buffers(void) {
	mgl_ctx_t* c = g_ctx;
	int y, x0, y0, y1, vw;
	if (c == NULL || c->buf == NULL || c->backbuf == NULL)
		return;

	x0 = c->vx; y0 = c->vy; y1 = c->vy + c->vh;
	if (x0 < 0) x0 = 0;
	if (y0 < 0) y0 = 0;
	if (y1 > c->height) y1 = c->height;
	vw = c->vw;
	if (x0 + vw > c->width) vw = c->width - x0;
	if (vw <= 0)
		return;

	for (y = y0; y < y1; y++)
		memcpy(c->buf     + (size_t)y * c->pitch      + (size_t)x0 * 4,
		       c->backbuf + (size_t)y * c->back_pitch + (size_t)x0 * 4,
		       (size_t)vw * 4);
}

static inline int fclampi(float v, int lo, int hi) {
	int i = (int)v;
	if (i < lo) return lo;
	if (i > hi) return hi;
	return i;
}

static void raster_tri(mgl_ctx_t* c, const mgl_vertex_t* a,
                        const mgl_vertex_t* b, const mgl_vertex_t* cc) {
	float area, inv_area;
	int minx, maxx, miny, maxy, x, y;

	if (a->clipped || b->clipped || cc->clipped)
		return;

	area = (b->x - a->x) * (cc->y - a->y) - (b->y - a->y) * (cc->x - a->x);
	if (fabsf(area) < 1e-6f)
		return;
	if (c->cull_face && area <= 0.0f)   /* CCW front-facing, GL default */
		return;
	inv_area = 1.0f / area;

	minx = fclampi(fminf(a->x, fminf(b->x, cc->x)), 0, c->width - 1);
	maxx = fclampi(fmaxf(a->x, fmaxf(b->x, cc->x)), 0, c->width - 1);
	miny = fclampi(fminf(a->y, fminf(b->y, cc->y)), 0, c->height - 1);
	maxy = fclampi(fmaxf(a->y, fmaxf(b->y, cc->y)), 0, c->height - 1);

	for (y = miny; y <= maxy; y++) {
		for (x = minx; x <= maxx; x++) {
			float px = (float)x + 0.5f, py = (float)y + 0.5f;
			float e_bc = (cc->x - b->x) * (py - b->y) - (cc->y - b->y) * (px - b->x);
			float e_ca = (a->x - cc->x) * (py - cc->y) - (a->y - cc->y) * (px - cc->x);
			float e_ab = (b->x - a->x) * (py - a->y) - (b->y - a->y) * (px - a->x);
			float wa, wb, wcc, depth, r, g, bl;
			uint32_t col;

			if (area > 0.0f) {
				if (e_bc < 0.0f || e_ca < 0.0f || e_ab < 0.0f) continue;
			} else {
				if (e_bc > 0.0f || e_ca > 0.0f || e_ab > 0.0f) continue;
			}

			wa = e_bc * inv_area; wb = e_ca * inv_area; wcc = e_ab * inv_area;
			depth = wa * a->depth + wb * b->depth + wcc * cc->depth;
			r  = wa * a->r + wb * b->r + wcc * cc->r;
			g  = wa * a->g + wb * b->g + wcc * cc->g;
			bl = wa * a->b + wb * b->b + wcc * cc->b;

			if (r < 0.0f) r = 0.0f;
			if (r > 1.0f) r = 1.0f;
			if (g < 0.0f) g = 0.0f;
			if (g > 1.0f) g = 1.0f;
			if (bl < 0.0f) bl = 0.0f;
			if (bl > 1.0f) bl = 1.0f;

			col = ((uint32_t)(r * 255.0f) << 16) |
			      ((uint32_t)(g * 255.0f) << 8) |
			      (uint32_t)(bl * 255.0f);
			put_px_depth(c, x, y, depth, col);
		}
	}
}

static void raster_line(mgl_ctx_t* c, const mgl_vertex_t* a, const mgl_vertex_t* b) {
	int x0 = (int)a->x, y0 = (int)a->y, x1 = (int)b->x, y1 = (int)b->y;
	int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
	int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
	int err = dx + dy, e2, steps = 0, total = dx - dy + 1;
	uint32_t colA = ((uint32_t)(a->r * 255.0f) << 16) | ((uint32_t)(a->g * 255.0f) << 8) | (uint32_t)(a->b * 255.0f);

	if (a->clipped || b->clipped)
		return;
	for (;;) {
		float t = total > 1 ? (float)steps / (float)total : 0.0f;
		float depth = a->depth + (b->depth - a->depth) * t;
		put_px_depth(c, x0, y0, depth, colA);
		if (x0 == x1 && y0 == y1) break;
		e2 = 2 * err;
		if (e2 >= dy) { err += dy; x0 += sx; }
		if (e2 <= dx) { err += dx; y0 += sy; }
		steps++;
	}
}

void mgl_end(void) {
	mgl_ctx_t* c = g_ctx;
	int i, n;
	if (c == NULL || !c->in_begin)
		return;
	c->in_begin = 0;
	n = c->nverts;

	switch (c->prim) {
	case MGL_POINTS:
		for (i = 0; i < n; i++) {
			const mgl_vertex_t* v = &c->verts[i];
			uint32_t col;
			if (v->clipped) continue;
			col = ((uint32_t)(v->r * 255.0f) << 16) | ((uint32_t)(v->g * 255.0f) << 8) | (uint32_t)(v->b * 255.0f);
			put_px_depth(c, (int)v->x, (int)v->y, v->depth, col);
		}
		break;
	case MGL_LINES:
		for (i = 0; i + 1 < n; i += 2)
			raster_line(c, &c->verts[i], &c->verts[i + 1]);
		break;
	case MGL_LINE_LOOP:
		for (i = 0; i < n; i++)
			raster_line(c, &c->verts[i], &c->verts[(i + 1) % n]);
		break;
	case MGL_TRIANGLES:
		for (i = 0; i + 2 < n; i += 3)
			raster_tri(c, &c->verts[i], &c->verts[i + 1], &c->verts[i + 2]);
		break;
	case MGL_TRIANGLE_STRIP:
		for (i = 0; i + 2 < n; i++) {
			if (i & 1)
				raster_tri(c, &c->verts[i + 1], &c->verts[i], &c->verts[i + 2]);
			else
				raster_tri(c, &c->verts[i], &c->verts[i + 1], &c->verts[i + 2]);
		}
		break;
	case MGL_TRIANGLE_FAN:
		for (i = 1; i + 1 < n; i++)
			raster_tri(c, &c->verts[0], &c->verts[i], &c->verts[i + 1]);
		break;
	case MGL_QUADS:
		for (i = 0; i + 3 < n; i += 4) {
			raster_tri(c, &c->verts[i], &c->verts[i + 1], &c->verts[i + 2]);
			raster_tri(c, &c->verts[i], &c->verts[i + 2], &c->verts[i + 3]);
		}
		break;
	}
	c->nverts = 0;
}

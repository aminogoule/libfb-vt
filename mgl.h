#ifndef MGL_H
#define MGL_H

#include <stddef.h>
#include <stdint.h>

/*
 * mgl -- a tiny software-only OpenGL-1.x-style immediate-mode API.
 *
 * Pure CPU rasterization into a caller-owned 32bpp (0x00RRGGBB) buffer, same
 * fb_drawbuf()/fb_pitch() buffer any other libfb-vt client draws into. No
 * GPU/DRM/KMS/vmwgfx involved -- this is the "programmatic" (software) GL
 * step; a real hardware-accelerated backend riding SVGA3D is a separate,
 * much larger effort (guest-backed surfaces, command buffers, a shader
 * compiler) left for later.
 *
 * Scope, deliberately minimal:
 *   - immediate mode only (glBegin/glEnd), no vertex arrays/VBOs
 *   - classic fixed-function matrix stack (GL_MODELVIEW / GL_PROJECTION)
 *   - flat/Gouraud triangle rasterization with a float depth buffer
 *   - screen-space (not perspective-correct) color/depth interpolation
 *   - no texturing, no GL_LIGHTING, no clipping beyond a whole-primitive
 *     near-plane reject (a triangle straddling w<=0 is dropped, not split)
 *
 * Single current context (a real global, same as desktop GL's implicit
 * per-thread current context) -- fine for one render loop per process.
 */

typedef float mgl_mat4[16];

typedef enum {
	MGL_MODELVIEW,
	MGL_PROJECTION,
} mgl_matrixmode_t;

typedef enum {
	MGL_POINTS,
	MGL_LINES,
	MGL_LINE_LOOP,
	MGL_TRIANGLES,
	MGL_TRIANGLE_STRIP,
	MGL_TRIANGLE_FAN,
	MGL_QUADS,
} mgl_prim_t;

typedef struct mgl_ctx mgl_ctx_t;

/* Create/destroy a context. width/height size the internal depth buffer;
   mgl_set_target() may later point it at a differently-sized buffer (e.g.
   after fb_resize()), which reallocates the depth buffer as needed. */
mgl_ctx_t* mgl_create(int width, int height);
void       mgl_destroy(mgl_ctx_t* ctx);
void       mgl_make_current(mgl_ctx_t* ctx);

/* Point the context at a draw buffer (32bpp, 0x00RRGGBB, row pitch in
   bytes) and set the full-buffer viewport. Call every frame -- the
   backend's back-buffer pointer can move (realloc on resize). */
void mgl_set_target(uint8_t* buf, size_t pitch, int width, int height);
void mgl_viewport(int x, int y, int w, int h);

/* ---- state ---- */
void mgl_clear_color(float r, float g, float b, float a);
void mgl_clear(int color, int depth);
void mgl_enable_depth_test(int on);
void mgl_enable_cull_face(int on);

/* ---- matrix stack ---- */
void mgl_matrix_mode(mgl_matrixmode_t m);
void mgl_load_identity(void);
void mgl_push_matrix(void);
void mgl_pop_matrix(void);
void mgl_mult_matrix(const mgl_mat4 m);
void mgl_translate(float x, float y, float z);
void mgl_rotate(float deg, float x, float y, float z);
void mgl_scale(float x, float y, float z);
void mgl_frustum(float l, float r, float b, float t, float n, float f);
void mgl_ortho(float l, float r, float b, float t, float n, float f);
void mgl_perspective(float fovy_deg, float aspect, float znear, float zfar);

/* ---- immediate mode ---- */
void mgl_begin(mgl_prim_t prim);
void mgl_color3f(float r, float g, float b);
void mgl_vertex3f(float x, float y, float z);
void mgl_end(void);

#endif /* MGL_H */

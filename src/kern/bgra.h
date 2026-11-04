/* bgra.h: colour for a port that was grey. The motion field stays AMD's, built on luma; what is new is getting luma out of a screen's packed BGRx and getting a whole colour picture back in.
 *
 * A frame is laid out as a vt_plane is, with the same padding, a pixel being one 32-bit word where a plane's sample is one F32: VT_PAD_X pixels each side, VT_PAD_Y rows above and below, rows of whole 64 B lines. So a tile's row at a
 * column that is a multiple of 16 is exactly one line and exactly one register.
 *
 * Two forms of each step:
 *   _vx        the fused form: bytes in 32-bit lanes, 16 pixels a register.
 *   _plain_vx  the plain form it must beat by measurement: luma by three multiplies; a picture by three Q6 F32 planes through the port's own fg_blend, split and merged around it. */
#ifndef MF_BGRA_H
#define MF_BGRA_H

#include <stddef.h>
#include <stdint.h>

#include "framegen.h"
#include "plane.h"

struct bgra {
    uint32_t *p; /* pixel (0, 0) */
    int w, h;
    int stride; /* in pixels */
};

static inline size_t bgra_bytes(int w, int h) { return plane_bytes(w, h); }
static inline struct bgra bgra_at(void *mem, int w, int h)
{
    const int s = plane_stride(w);
    struct bgra f = {(uint32_t *)mem + (size_t)s * VT_PAD_Y + VT_PAD_X, w, h, s};
    return f;
}
static inline uint32_t *bgra_row(const struct bgra *f, int y) { return f->p + (ptrdiff_t)y * f->stride; }
/* Where bgra_at's memory starts, for a whole frame's DMA. */
static inline void *bgra_base(const struct bgra *f) { return f->p - (size_t)f->stride * VT_PAD_Y - VT_PAD_X; }

/* Once, before any blend: the weights' table. 0, or -1 if the table fails its own check. */
int bgra_init(void);

/* Edge pixels into the padding: rows y0 .. y1 left and right; then the rows above and below, once. */
void bgra_pad_rows(const struct bgra *f, int y0, int y1);
void bgra_pad_tb(const struct bgra *f);

/* Q6 luma, (29 B + 150 G + 77 R + 2) >> 2, integer-valued F32, at most 16,320. */
void bgra_luma_vx(const struct vt_plane *dst, const struct bgra *src, int y0, int y1);
void bgra_luma_plain_vx(const struct vt_plane *dst, const struct bgra *src, int y0, int y1);
void bgra_luma_ref(const struct vt_plane *dst, const struct bgra *src, int y0, int y1);

/* The generated picture from the mid vectors, 16 x 16 tiles numbered row by row: the same tiles, the same vectors, the same triangle windows as fg_blend. */
void bgra_blend_vx(const struct fg_field *mid, const struct bgra *a, const struct bgra *b, const struct bgra *out, int t0, int t1);

/* The plain form's split and merge: B, G, R planes in Q6, and back. */
void bgra_split_vx(const struct vt_plane dst[3], const struct bgra *src, int y0, int y1);
void bgra_merge_vx(const struct bgra *out, const struct vt_plane src[3], int y0, int y1);

#endif

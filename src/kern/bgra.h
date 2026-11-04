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

/* The generated picture at full resolution, a vector per pixel. field: vectors at the middle instant over cells of stride x stride pixels of the picture itself (8, or finer by bgra_fine_vx). ya, yb: the two frames' luma. Each pixel picks, among the vectors of the blocks around its tile, the one on which the two
 * frames agree best over a window of (2 r + 1) rows by (2 h + 1) columns, h at most 2; a vector other than none must beat it by bias (Q6 counts over the window). Samples are whole pixels of both frames, nothing is resampled: a at x + (v >> 1), b at
 * x - (v - (v >> 1)), which are the same place of the scene, so the mean of the two is as sharp as they are. parent: the field a level up, whose block is the tile, and whose vector wins a tie: on a flat panel with a mark on it, it knows the panel moves. */
/* A finer grid of vectors, cells of stride x stride pixels, stride 2 or 4: each cell weighs the mid vectors of the blocks around it, the larger block's, no motion, and the eight one-pixel steps around the best, over 8 x 8 centred on the cell, where the two
 * frames meet at the middle instant. A local search: it cannot make the far false matches a small block invites. */
void bgra_fine_vx(const struct fg_field *mid, const struct fg_field *parent, const struct vt_plane *ya, const struct vt_plane *yb, const struct fg_field *fine, int stride, int i0, int i1);
void bgra_pick_vx(const struct fg_field *field, int stride, const struct fg_field *parent, const struct vt_plane *ya, const struct vt_plane *yb, const struct bgra *a, const struct bgra *b, const struct bgra *out, int r, int h, float bias, int phase, int t0, int t1);

/* dst = src plus, on each colour, what FSR changed in its luma: color (0 .. 1) against y (Q6). */
void bgra_fsr_merge_vx(const struct bgra *src, const struct vt_plane *y, const struct vt_plane *color, const struct bgra *dst, int y0, int y1);

/* The plain form's split and merge: B, G, R planes in Q6, and back. */
void bgra_split_vx(const struct vt_plane dst[3], const struct bgra *src, int y0, int y1);
void bgra_merge_vx(const struct bgra *out, const struct vt_plane src[3], int y0, int y1);

#endif

/* bgra.c: see bgra.h. Against vxu.h: native MVEX on the card, the lane model on the host, and the two must agree bit for bit (mf_replay --ref). */
#include "bgra.h"

#include <string.h>

#include "img.h"
#include "vxu.h"

#define VLOOP16(x, w) for (int x = 0; x < (((w) + 15) & ~15); x += 16)

static const uint32_t M_00FF00FF VX_ALIGNED = 0x00ff00ffu, M_FF00FF00 VX_ALIGNED = 0xff00ff00u, M_7F VX_ALIGNED = 0x7f7f7f7fu, M_FF VX_ALIGNED = 0x000000ffu;
static const uint32_t ROUND_8 VX_ALIGNED = 0x00800080u, ROUND_2 VX_ALIGNED = 2u, ALPHA VX_ALIGNED = 0xff000000u;
static const uint32_t ABS_F VX_ALIGNED = 0x7fffffffu;
/* B + (R << 16), times this, holds 29 B + 77 R in its high half: one multiply for two channels. 77 B stays under 2^16 so nothing carries in; 29 R falls off the top. */
static const uint32_t K_BR VX_ALIGNED = (29u << 16) | 77u, K_G VX_ALIGNED = 150u, K_B VX_ALIGNED = 29u, K_R VX_ALIGNED = 77u;

/* Per row of a tile, per fetch (the block row: own, other; the block column: left, own, right), per lane: in 256ths, summing to 256 on every lane of every row. fg_blend's triangles, whose products are in 1024ths, rounded, and what the
 * rounding lost put back on the largest weight. Summing to exactly 256 is what lets 8-bit samples multiply and add in 16-bit fields of a 32-bit lane: 255 x 256 + 128 < 2^16, so one field never carries into the next. */
static int32_t W_TAB[16][6][16] VX_ALIGNED;

int bgra_init(void)
{
    static const int W_LEFT[16] = {15, 13, 11, 9, 7, 5, 3, 1, 0, 0, 0, 0, 0, 0, 0, 0};
    static const int W_SELF[16] = {17, 19, 21, 23, 25, 27, 29, 31, 31, 29, 27, 25, 23, 21, 19, 17};
    static const int W_RIGHT[16] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 3, 5, 7, 9, 11, 13, 15};
    const int *const wh[3] = {W_LEFT, W_SELF, W_RIGHT};
    for (int s = 0; s < 16; s++) {
        const int w_other = s < 8 ? 15 - 2 * s : 2 * s - 15, w_self = 32 - w_other;
        for (int lane = 0; lane < 16; lane++) {
            int sum = 0;
            for (int a = 0; a < 2; a++)
                for (int b = 0; b < 3; b++) {
                    const int v = (a ? w_other : w_self) * wh[b][lane];
                    const int q = (v + 2) >> 2;
                    W_TAB[s][a * 3 + b][lane] = q;
                    sum += q;
                }
            W_TAB[s][1][lane] += 256 - sum;
        }
    }
    for (int s = 0; s < 16; s++)
        for (int lane = 0; lane < 16; lane++) {
            int sum = 0;
            for (int k = 0; k < 6; k++) {
                if (W_TAB[s][k][lane] < 0)
                    return -1;
                sum += W_TAB[s][k][lane];
            }
            if (sum != 256)
                return -1;
        }
    return 0;
}

void bgra_pad_rows(const struct bgra *f, int y0, int y1)
{
    for (int y = y0; y < y1; y++) {
        uint32_t *r = bgra_row(f, y);
        const uint32_t l = r[0], e = r[f->w - 1];
        for (int x = -VT_PAD_X; x < 0; x++)
            r[x] = l;
        const int end = ((f->w + 15) & ~15) + VT_PAD_X;
        for (int x = f->w; x < end; x++)
            r[x] = e;
    }
}

void bgra_pad_tb(const struct bgra *f)
{
    const size_t bytes = (size_t)f->stride * 4u;
    const uint32_t *top = bgra_row(f, 0) - VT_PAD_X, *bot = bgra_row(f, f->h - 1) - VT_PAD_X;
    for (int k = 1; k <= VT_PAD_Y; k++) {
        memcpy(bgra_row(f, -k) - VT_PAD_X, top, bytes);
        memcpy(bgra_row(f, f->h - 1 + k) - VT_PAD_X, bot, bytes);
    }
}

void bgra_luma_vx(const struct vt_plane *dst, const struct bgra *src, int y0, int y1)
{
    const vxu kbr = vxu_bcast(&K_BR), kg = vxu_bcast(&K_G);
    for (int y = y0; y < y1; y++) {
        const uint32_t *s = bgra_row(src, y);
        float *d = plane_row(dst, y);
        VLOOP16(x, dst->w) {
            const vxu p = vxu_load(s + x);
            const vxu br = vxu_srl(vxu_mull(vxu_and_s(p, &M_00FF00FF), kbr), 16);
            const vxu g = vxu_mull(vxu_and_s(vxu_srl(p, 8), &M_FF), kg);
            vx_store_nr(d + x, vxu_to_f(vxu_srl(vxu_add_s(vxu_add(br, g), &ROUND_2), 2)));
        }
    }
    img_fence();
}

void bgra_luma_plain_vx(const struct vt_plane *dst, const struct bgra *src, int y0, int y1)
{
    const vxu kb = vxu_bcast(&K_B), kg = vxu_bcast(&K_G), kr = vxu_bcast(&K_R);
    for (int y = y0; y < y1; y++) {
        const uint32_t *s = bgra_row(src, y);
        float *d = plane_row(dst, y);
        VLOOP16(x, dst->w) {
            const vxu p = vxu_load(s + x);
            const vxu b = vxu_mull(vxu_and_s(p, &M_FF), kb), g = vxu_mull(vxu_and_s(vxu_srl(p, 8), &M_FF), kg), r = vxu_mull(vxu_and_s(vxu_srl(p, 16), &M_FF), kr);
            vx_store_nr(d + x, vxu_to_f(vxu_srl(vxu_add_s(vxu_add(vxu_add(b, g), r), &ROUND_2), 2)));
        }
    }
    img_fence();
}

void bgra_luma_ref(const struct vt_plane *dst, const struct bgra *src, int y0, int y1)
{
    for (int y = y0; y < y1; y++) {
        const uint32_t *s = bgra_row(src, y);
        float *d = plane_row(dst, y);
        for (int x = 0; x < ((dst->w + 15) & ~15); x++) {
            const uint32_t p = s[x];
            d[x] = (float)((29u * (p & 255u) + 150u * ((p >> 8) & 255u) + 77u * ((p >> 16) & 255u) + 2u) >> 2);
        }
    }
}

static inline int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

/* A row of a frame from column x, for a load of 16 pixels that stays inside the padding. Rows beyond the padding repeat its last. */
static inline const uint32_t *row_at(const struct bgra *f, int x, int y)
{
    const int last = ((f->w + 15) & ~15) + VT_PAD_X - 16;
    return bgra_row(f, clampi(y, -VT_PAD_Y, f->h + VT_PAD_Y - 1)) + clampi(x, -VT_PAD_X, last);
}

/* The byte-wise mean of two registers of pixels, rounded up, 16 pixels at once with no branch and no unpacking: x | y is x + y less x & y, and half of x ^ y is what the bits on which they differ add to the mean. The mask stops a shifted
 * bit crossing into the byte below. */
static inline vxu mean_bytes(vxu a, vxu b) { return vxu_sub(vxu_or(a, b), vxu_and_s(vxu_srl(vxu_xor(a, b), 1), &M_7F)); }

void bgra_blend_vx(const struct fg_field *mid, const struct bgra *a, const struct bgra *b, const struct bgra *out, int t0, int t1)
{
    const int tw = (out->w + 15) / 16;
    for (int t = t0; t < t1; t++) {
        const int tx = t % tw, ty = t / tw, x = tx * 16, y0 = ty * 16;
        const int y1 = y0 + 16 < out->h ? y0 + 16 : out->h;
        /* The nine blocks around, packed: comparing them is comparing words. */
        uint32_t near[3][3], any = 0;
        for (int yy = 0; yy < 3; yy++)
            for (int xx = 0; xx < 3; xx++) {
                const struct fg_vec *v = &mid->v[clampi(ty + yy - 1, 0, mid->h - 1) * mid->w + clampi(tx + xx - 1, 0, mid->w - 1)];
                memcpy(&near[yy][xx], v, sizeof near[yy][xx]);
            }
        for (int yy = 0; yy < 3; yy++)
            for (int xx = 0; xx < 3; xx++)
                any |= near[yy][xx] ^ near[0][0];
        if (!any) {
            /* Most of a picture. One vector, half of it each way at full resolution: whole pixels, since the field is at half resolution. */
            struct fg_vec v;
            memcpy(&v, &near[0][0], sizeof v);
            for (int y = y0; y < y1; y++)
                vxu_store_nr(bgra_row(out, y) + x, mean_bytes(vxu_loadu(row_at(a, x + v.x, y + v.y)), vxu_loadu(row_at(b, x - v.x, y - v.y))));
            continue;
        }
        for (int y = y0; y < y1; y++) {
            const int s = y - y0;
            const uint32_t *const row_other = near[s < 8 ? 0 : 2], *const rows[2] = {near[1], row_other};
            vxu rb = vxu_zero(), ag = rb;
            for (int k = 0; k < 6; k++) {
                struct fg_vec v;
                memcpy(&v, &rows[k / 3][k % 3], sizeof v);
                const vxu m = mean_bytes(vxu_loadu(row_at(a, x + v.x, y + v.y)), vxu_loadu(row_at(b, x - v.x, y - v.y)));
                const vxu w = vxu_load((const uint32_t *)W_TAB[s][k]);
                rb = vxu_add(rb, vxu_mull(vxu_and_s(m, &M_00FF00FF), w));
                ag = vxu_add(ag, vxu_mull(vxu_and_s(vxu_srl(m, 8), &M_00FF00FF), w));
            }
            const vxu lo = vxu_and_s(vxu_srl(vxu_add_s(rb, &ROUND_8), 8), &M_00FF00FF), hi = vxu_and_s(vxu_add_s(ag, &ROUND_8), &M_FF00FF00);
            vxu_store_nr(bgra_row(out, y) + x, vxu_or(lo, hi));
        }
    }
    img_fence();
}

void bgra_split_vx(const struct vt_plane dst[3], const struct bgra *src, int y0, int y1)
{
    static const float Q6 VX_ALIGNED = (float)VT_Q6;
    for (int y = y0; y < y1; y++) {
        const uint32_t *s = bgra_row(src, y);
        float *db = plane_row(&dst[0], y), *dg = plane_row(&dst[1], y), *dr = plane_row(&dst[2], y);
        VLOOP16(x, dst[0].w) {
            const vxu p = vxu_load(s + x);
            vx_store_nr(db + x, vx_mul_s(vxu_to_f(vxu_and_s(p, &M_FF)), &Q6));
            vx_store_nr(dg + x, vx_mul_s(vxu_to_f(vxu_and_s(vxu_srl(p, 8), &M_FF)), &Q6));
            vx_store_nr(dr + x, vx_mul_s(vxu_to_f(vxu_and_s(vxu_srl(p, 16), &M_FF)), &Q6));
        }
    }
    img_fence();
}

void bgra_merge_vx(const struct bgra *out, const struct vt_plane src[3], int y0, int y1)
{
    static const float ONE_64 VX_ALIGNED = 1.0f / (float)VT_Q6;
    const vxu alpha = vxu_bcast(&ALPHA);
    for (int y = y0; y < y1; y++) {
        const float *sb = plane_row(&src[0], y), *sg = plane_row(&src[1], y), *sr = plane_row(&src[2], y);
        uint32_t *d = bgra_row(out, y);
        VLOOP16(x, src[0].w) {
            const vxu b = vxu_from_f(vx_round(vx_mul_s(vx_load(sb + x), &ONE_64), VX_RND_NEAREST));
            const vxu g = vxu_from_f(vx_round(vx_mul_s(vx_load(sg + x), &ONE_64), VX_RND_NEAREST));
            const vxu r = vxu_from_f(vx_round(vx_mul_s(vx_load(sr + x), &ONE_64), VX_RND_NEAREST));
            vxu_store_nr(d + x, vxu_or(vxu_or(b, vxu_sll(g, 8)), vxu_or(vxu_sll(r, 16), alpha)));
        }
    }
    img_fence();
}

/* ---- the picture at full resolution, a vector per pixel ---- */

#define PICK_MAX 10    /* vectors a tile weighs */
#define PICK_RADIUS 3  /* rows of the window each side, at most */
#define FINE_FAVOUR_ZERO 64.0f /* Q6 counts over 8 x 8: one grey level over the window, for still things */
#define FINE_ENOUGH 256.0f     /* Q6 counts over 8 x 8: under this a cell needs no step around its best */

static inline const float *luma_at(const struct vt_plane *p, int x, int y)
{
    const int last = ((p->w + 15) & ~15) + VT_PAD_X - 16;
    return plane_row(p, clampi(y, -VT_PAD_Y, p->h + VT_PAD_Y - 1)) + clampi(x, -VT_PAD_X, last);
}

/* Which of the 16 lanes from column x of row y lie inside the picture, as a mask. A load whose start was clamped to the padding has no lane inside, so the clamp cannot misplace one. */
static inline unsigned inside(int x, int y, int w, int h)
{
    if (y < 0 || y >= h)
        return 0u;
    const int lo = x < 0 ? -x : 0, hi = w - x < 16 ? w - x : 16;
    return hi <= lo ? 0u : ((1u << hi) - 1u) & ~((1u << lo) - 1u);
}

/* A vector decoded: a at x + ax, y + ay; b at x - bx, y - by. clip: some sample of this tile falls outside the picture for one of the frames. */
struct pick_v {
    int ax, ay, bx, by, clip;
    int odd_x, odd_y; /* the total motion is odd this way: the true middle lies between two whole pixels */
};

static inline void pick_decode(struct pick_v *c, uint32_t w)
{
    struct fg_vec v;
    memcpy(&v, &w, sizeof v);
    c->ax = v.x >> 1;
    c->ay = v.y >> 1;
    c->bx = v.x - c->ax;
    c->by = v.y - c->ay;
    c->clip = 0;
    c->odd_x = v.x & 1;
    c->odd_y = v.y & 1;
}

static inline void pick_clip(struct pick_v *c, int x, int y0, int y1, int r, int w, int h)
{
    c->clip = x - 16 + c->ax < 0 || x + 32 + c->ax > w || y0 - r + c->ay < 0 || y1 + r + c->ay > h || x - 16 - c->bx < 0 || x + 32 - c->bx > w || y0 - r - c->by < 0 || y1 + r - c->by > h;
}

/* The picture's 16 pixels for a vector. Where only one frame has the place, as what scrolls in at the picture's edge has, that frame alone: exact, where the mean would take half of it from the padding. */
static inline vxu pick_pair(const struct bgra *a, const struct bgra *b, const struct bgra *out, int x, int y, const struct pick_v *c)
{
    const vxu pa = vxu_loadu(row_at(a, x + c->ax, y + c->ay)), pb = vxu_loadu(row_at(b, x - c->bx, y - c->by));
    vxu m = mean_bytes(pa, pb);
    if (c->clip) {
        const unsigned ka = inside(x + c->ax, y + c->ay, out->w, out->h), kb = inside(x - c->bx, y - c->by, out->w, out->h);
        m = vxu_blend((vxk)(kb & ~ka), m, pb);
        m = vxu_blend((vxk)(ka & ~kb), m, pa);
    }
    return m;
}

/* The mean rounded down, for the second of two means: one up, one down, and the rounding does not creep. */
static inline vxu mean_floor(vxu a, vxu b) { return vxu_add(vxu_and(a, b), vxu_and_s(vxu_srl(vxu_xor(a, b), 1), &M_7F)); }

/* The picture along a vector. A pair a(x + ax), b(x - bx) is one place of the scene, but at an odd total motion it lies half a pixel from the true middle, and the next pair over half a pixel the other side. The mean of the two pairs sits on the true middle: the
 * picture that really was there, as nearly as whole pixels of two frames can give it. phase 0: the first pair alone. */
static inline vxu pick_px(const struct bgra *a, const struct bgra *b, const struct bgra *out, int x, int y, const struct pick_v *c, int phase)
{
    const vxu m = pick_pair(a, b, out, x, y, c);
    if (!phase || !(c->odd_x | c->odd_y))
        return m;
    if (!c->odd_y)
        return mean_floor(m, pick_pair(a, b, out, x + 1, y, c));
    if (!c->odd_x)
        return mean_floor(m, pick_pair(a, b, out, x, y + 1, c));
    return mean_floor(vxu_sub(vxu_or(m, pick_pair(a, b, out, x + 1, y + 1, c)), vxu_and_s(vxu_srl(vxu_xor(m, pick_pair(a, b, out, x + 1, y + 1, c)), 1), &M_7F)), mean_bytes(pick_pair(a, b, out, x + 1, y, c), pick_pair(a, b, out, x, y + 1, c)));
}

/* The two frames' disagreement along a vector over 16 columns. Where one frame has no such place there is nothing to disagree with. */
static inline vxf pick_d(const struct vt_plane *ya, const struct vt_plane *yb, int x, int y, const struct pick_v *c, int w, int h)
{
    const vxf d = vx_abs(vx_sub(vx_loadu(luma_at(ya, x + c->ax, y + c->ay)), vx_loadu(luma_at(yb, x - c->bx, y - c->by))), &ABS_F);
    if (!c->clip)
        return d;
    return vx_blend((vxk)(inside(x + c->ax, y + c->ay, w, h) & inside(x - c->bx, y - c->by, w, h)), vx_zero(), d);
}

/* How well the two frames agree over 8 x 8 at the middle instant along v. */
static inline float fine_sad(const struct vt_plane *ya, const struct vt_plane *yb, int x, int y, int vx_, int vy_)
{
    static const unsigned short EIGHT = 0x00ff;
    const int ax = vx_ >> 1, ay = vy_ >> 1, bx = vx_ - ax, by = vy_ - ay;
    const vxf zero = vx_zero();
    vxf sum = zero;
    for (int r = 0; r < 8; r++)
        sum = vx_add(sum, vx_blend(EIGHT, zero, vx_abs(vx_sub(vx_loadu(luma_at(ya, x + ax, y + r + ay)), vx_loadu(luma_at(yb, x - bx, y + r - by))), &ABS_F)));
    return vx_hsum(sum);
}

void bgra_fine_vx(const struct fg_field *mid, const struct fg_field *parent, const struct vt_plane *ya, const struct vt_plane *yb, const struct fg_field *fine, int stride, int i0, int i1)
{
    for (int i = i0; i < i1; i++) {
        const int cx = i % fine->w, cy = i / fine->w;
        const int x = cx * stride + stride / 2 - 4, y = cy * stride + stride / 2 - 4;
        const int bx = (x + 4) / 8, by = (y + 4) / 8;
        /* No motion first, with an edge: a still thing stays still. */
        float best = fine_sad(ya, yb, x, y, 0, 0) - FINE_FAVOUR_ZERO;
        int bestx = 0, besty = 0;
        uint32_t seen[10];
        int n = 0;
        const struct fg_vec *pv = &parent->v[clampi(by / 2, 0, parent->h - 1) * parent->w + clampi(bx / 2, 0, parent->w - 1)];
        for (int k = 0; k < 10; k++) {
            /* The block's own and the eight around it, then the larger block's. */
            struct fg_vec v;
            if (k == 9) {
                v.x = (int16_t)(pv->x * 2);
                v.y = (int16_t)(pv->y * 2);
            } else {
                v = mid->v[clampi(by + (k % 3) - 1, 0, mid->h - 1) * mid->w + clampi(bx + (k / 3) - 1, 0, mid->w - 1)];
            }
            uint32_t w;
            memcpy(&w, &v, sizeof w);
            int dup = !w;
            for (int q = 0; q < n; q++)
                dup |= seen[q] == w;
            if (dup)
                continue;
            seen[n++] = w;
            const float s = fine_sad(ya, yb, x, y, v.x, v.y);
            if (s < best) {
                best = s;
                bestx = v.x;
                besty = v.y;
            }
        }
        /* The steps a block's whole-pixel answer may have missed here: the eight around the best, one pixel of total motion each way. */
        if (best > FINE_ENOUGH) {
            const int cxv = bestx, cyv = besty;
            for (int dy = -1; dy < 2; dy++)
                for (int dx = -1; dx < 2; dx++) {
                    if (!dx && !dy)
                        continue;
                    const float s = fine_sad(ya, yb, x, y, cxv + dx, cyv + dy);
                    if (s < best) {
                        best = s;
                        bestx = cxv + dx;
                        besty = cyv + dy;
                    }
                }
        }
        fine->v[i].x = (int16_t)bestx;
        fine->v[i].y = (int16_t)besty;
    }
}

void bgra_pick_vx(const struct fg_field *field, int stride, const struct fg_field *parent, const struct vt_plane *ya, const struct vt_plane *yb, const struct bgra *a, const struct bgra *b, const struct bgra *out, int r, int h, float bias, int phase, int t0, int t1)
{
    /* Per vector, per row of the tile and of the window beyond it: the disagreement over the row's own columns. Integer-valued F32: 16,320 x 7 x 5 is far below 2^24, so the running sums below are exact, and the card and the lane model agree. */
    float cols[PICK_MAX][16 + 2 * PICK_RADIUS][16] VX_ALIGNED;
    const float bias_q VX_ALIGNED = bias;
    const vxf vbias = vx_bcast(&bias_q);
    const int tw = (out->w + 15) / 16;
    /* The cells of the tile, then those of a ring half a tile wide: nearest first, for when more vectors are about than a tile weighs. */
    const int inner = 16 / stride, ring = 8 / stride;
    for (int t = t0; t < t1; t++) {
        const int tx = t % tw, ty = t / tw, x = tx * 16, y0 = ty * 16;
        const int y1 = y0 + 16 < out->h ? y0 + 16 : out->h;
        const int c0x = tx * inner, c0y = ty * inner;
        uint32_t cand[PICK_MAX];
        /* First, and so the winner of a tie: the tile's own block of the level above, whose larger block sees more: on a flat panel with a mark on it, it knows the panel moves. */
        const struct fg_vec *pv = &parent->v[clampi(ty, 0, parent->h - 1) * parent->w + clampi(tx, 0, parent->w - 1)];
        const struct fg_vec ps = {(int16_t)(pv->x * 2), (int16_t)(pv->y * 2)};
        memcpy(&cand[0], &ps, sizeof cand[0]);
        int n = 1;
        for (int pass = 0; pass < 2; pass++)
            for (int cy = c0y - ring; cy < c0y + inner + ring; cy++)
                for (int cx = c0x - ring; cx < c0x + inner + ring; cx++) {
                    const int in_tile = cx >= c0x && cx < c0x + inner && cy >= c0y && cy < c0y + inner;
                    if (in_tile != (pass == 0))
                        continue;
                    uint32_t w;
                    memcpy(&w, &field->v[clampi(cy, 0, field->h - 1) * field->w + clampi(cx, 0, field->w - 1)], sizeof w);
                    int seen = 0;
                    for (int i = 0; i < n; i++)
                        seen |= cand[i] == w;
                    if (!seen && n < PICK_MAX)
                        cand[n++] = w;
                }
        struct pick_v c[PICK_MAX];
        if (n == 1) {
            /* Most of a picture. */
            pick_decode(&c[0], cand[0]);
            pick_clip(&c[0], x, y0, y1, 0, out->w, out->h);
            for (int y = y0; y < y1; y++)
                vxu_store_nr(bgra_row(out, y) + x, pick_px(a, b, out, x, y, &c[0], phase));
            continue;
        }
        /* No motion is always weighed. */
        {
            int z = -1;
            for (int i = 0; i < n; i++)
                if (!cand[i])
                    z = i;
            if (z < 0) {
                z = n < PICK_MAX ? n++ : PICK_MAX - 1;
                cand[z] = 0;
            }
        }
        for (int i = 0; i < n; i++) {
            pick_decode(&c[i], cand[i]);
            pick_clip(&c[i], x, y0, y1, r, out->w, out->h);
        }
        const int rows = y1 - y0;
        for (int i = 0; i < n; i++)
            for (int q = -r; q < rows + r; q++) {
                const int yq = y0 + q;
                const vxf cur = pick_d(ya, yb, x, yq, &c[i], out->w, out->h);
                vxf sum = cur;
                if (h > 0) {
                    const vxf prev = pick_d(ya, yb, x - 16, yq, &c[i], out->w, out->h), next = pick_d(ya, yb, x + 16, yq, &c[i], out->w, out->h);
                    sum = vx_add(sum, vx_add(vx_alignr(cur, prev, 15), vx_alignr(next, cur, 1)));
                    if (h > 1)
                        sum = vx_add(sum, vx_add(vx_alignr(cur, prev, 14), vx_alignr(next, cur, 2)));
                }
                vx_store(cols[i][q + r], sum);
            }
        vxf cost[PICK_MAX];
        for (int i = 0; i < n; i++) {
            vxf s = vx_load(cols[i][0]);
            for (int q = 1; q <= 2 * r; q++)
                s = vx_add(s, vx_load(cols[i][q]));
            cost[i] = s;
        }
        for (int s = 0; s < rows; s++) {
            const int y = y0 + s;
            /* The window of row s is cols[s .. s + 2 r]. */
            vxu px = pick_px(a, b, out, x, y, &c[0], phase);
            vxf best = cost[0];
            for (int i = 1; i < n; i++) {
                const vxf ci = vx_add(cost[i], vbias);
                const vxk k = vx_cmp(ci, best, VX_CMP_LT);
                if (k) {
                    px = vxu_blend(k, px, pick_px(a, b, out, x, y, &c[i], phase));
                    best = vx_blend(k, best, ci);
                }
            }
            vxu_store_nr(bgra_row(out, y) + x, px);
            if (s + 1 < rows)
                for (int i = 0; i < n; i++)
                    cost[i] = vx_add(vx_sub(cost[i], vx_load(cols[i][s])), vx_load(cols[i][s + 2 * r + 1]));
        }
    }
    img_fence();
}

/* ---- FSR is grey: its change in luma, onto the colours ---- */

void bgra_fsr_merge_vx(const struct bgra *src, const struct vt_plane *y, const struct vt_plane *color, const struct bgra *dst, int y0, int y1)
{
    static const float K255 VX_ALIGNED = 255.0f, ONE_64 VX_ALIGNED = 1.0f / (float)VT_Q6, K0 VX_ALIGNED = 0.0f;
    const vxf zero = vx_bcast(&K0), top = vx_bcast(&K255);
    const vxu alpha = vxu_bcast(&ALPHA);
    for (int r = y0; r < y1; r++) {
        const uint32_t *s = bgra_row(src, r);
        const float *ly = plane_row(y, r), *lc = plane_row(color, r);
        uint32_t *d = bgra_row(dst, r);
        VLOOP16(x, src->w) {
            const vxu p = vxu_load(s + x);
            const vxf delta = vx_sub(vx_mul_s(vx_load(lc + x), &K255), vx_mul_s(vx_load(ly + x), &ONE_64));
            const vxf b = vx_round(vx_min(vx_max(vx_add(vxu_to_f(vxu_and_s(p, &M_FF)), delta), zero), top), VX_RND_NEAREST);
            const vxf g = vx_round(vx_min(vx_max(vx_add(vxu_to_f(vxu_and_s(vxu_srl(p, 8), &M_FF)), delta), zero), top), VX_RND_NEAREST);
            const vxf c = vx_round(vx_min(vx_max(vx_add(vxu_to_f(vxu_and_s(vxu_srl(p, 16), &M_FF)), delta), zero), top), VX_RND_NEAREST);
            vxu_store_nr(d + x, vxu_or(vxu_or(vxu_from_f(b), vxu_sll(vxu_from_f(g), 8)), vxu_or(vxu_sll(vxu_from_f(c), 16), alpha)));
        }
    }
    img_fence();
}

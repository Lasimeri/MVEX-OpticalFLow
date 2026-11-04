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

#define PICK_MAX 8     /* vectors a tile weighs */
#define PICK_RADIUS 3  /* rows of the window each side, at most */

static inline const float *luma_at(const struct vt_plane *p, int x, int y)
{
    const int last = ((p->w + 15) & ~15) + VT_PAD_X - 16;
    return plane_row(p, clampi(y, -VT_PAD_Y, p->h + VT_PAD_Y - 1)) + clampi(x, -VT_PAD_X, last);
}

/* A tile's own four blocks first, then the ring around them: when more vectors are about than a tile weighs, the nearest stay. */
static const signed char PICK_ORDER[16][2] = {{0, 0}, {1, 0}, {0, 1}, {1, 1}, {-1, 0}, {-1, 1}, {2, 0}, {2, 1}, {0, -1}, {1, -1}, {0, 2}, {1, 2}, {-1, -1}, {2, -1}, {-1, 2}, {2, 2}};

void bgra_pick_vx(const struct fg_field *mid, const struct fg_field *parent, const struct vt_plane *ya, const struct vt_plane *yb, const struct bgra *a, const struct bgra *b, const struct bgra *out, int r, int h, float bias, int t0, int t1)
{
    /* Per vector, per row of the tile and of the window beyond it: the disagreement over the row's own columns. Integer-valued F32: 16,320 x 7 x 5 is far below 2^24, so the running sums below are exact, and the card and the lane model agree. */
    float cols[PICK_MAX][16 + 2 * PICK_RADIUS][16] VX_ALIGNED;
    const float bias_q VX_ALIGNED = bias;
    const vxf vbias = vx_bcast(&bias_q);
    const int tw = (out->w + 15) / 16;
    for (int t = t0; t < t1; t++) {
        const int tx = t % tw, ty = t / tw, x = tx * 16, y0 = ty * 16;
        const int y1 = y0 + 16 < out->h ? y0 + 16 : out->h;
        uint32_t cand[PICK_MAX];
        /* First, and so the winner of a tie: the tile's own block of the level above, whose larger block sees more: on a flat panel with a mark on it, it knows the panel moves. */
        const struct fg_vec *pv = &parent->v[clampi(ty, 0, parent->h - 1) * parent->w + clampi(tx, 0, parent->w - 1)];
        const struct fg_vec ps = {(int16_t)(pv->x * 2), (int16_t)(pv->y * 2)};
        memcpy(&cand[0], &ps, sizeof cand[0]);
        int n = 1;
        for (int o = 0; o < 16; o++) {
            uint32_t w;
            memcpy(&w, &mid->v[clampi(2 * ty + PICK_ORDER[o][1], 0, mid->h - 1) * mid->w + clampi(2 * tx + PICK_ORDER[o][0], 0, mid->w - 1)], sizeof w);
            int seen = 0;
            for (int i = 0; i < n; i++)
                seen |= cand[i] == w;
            if (!seen && n < PICK_MAX)
                cand[n++] = w;
        }
        /* Decoded once: a at x + ax, y + ay; b at x - bx, y - by. */
        int ax[PICK_MAX], ay[PICK_MAX], bx[PICK_MAX], by[PICK_MAX];
        if (n == 1) {
            /* Most of a picture. */
            struct fg_vec v;
            memcpy(&v, &cand[0], sizeof v);
            const int sax = v.x >> 1, say = v.y >> 1, sbx = v.x - sax, sby = v.y - say;
            for (int y = y0; y < y1; y++)
                vxu_store_nr(bgra_row(out, y) + x, mean_bytes(vxu_loadu(row_at(a, x + sax, y + say)), vxu_loadu(row_at(b, x - sbx, y - sby))));
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
            struct fg_vec v;
            memcpy(&v, &cand[i], sizeof v);
            ax[i] = v.x >> 1;
            ay[i] = v.y >> 1;
            bx[i] = v.x - ax[i];
            by[i] = v.y - ay[i];
        }
        const int rows = y1 - y0;
        for (int i = 0; i < n; i++) {
            for (int q = -r; q < rows + r; q++) {
                const int yq = y0 + q;
                const float *pa = luma_at(ya, x + ax[i], yq + ay[i]), *pb = luma_at(yb, x - bx[i], yq - by[i]);
                const vxf cur = vx_abs(vx_sub(vx_loadu(pa), vx_loadu(pb)), &ABS_F);
                vxf sum = cur;
                if (h > 0) {
                    const vxf prev = vx_abs(vx_sub(vx_loadu(luma_at(ya, x - 16 + ax[i], yq + ay[i])), vx_loadu(luma_at(yb, x - 16 - bx[i], yq - by[i]))), &ABS_F);
                    const vxf next = vx_abs(vx_sub(vx_loadu(luma_at(ya, x + 16 + ax[i], yq + ay[i])), vx_loadu(luma_at(yb, x + 16 - bx[i], yq - by[i]))), &ABS_F);
                    sum = vx_add(sum, vx_add(vx_alignr(cur, prev, 15), vx_alignr(next, cur, 1)));
                    if (h > 1)
                        sum = vx_add(sum, vx_add(vx_alignr(cur, prev, 14), vx_alignr(next, cur, 2)));
                }
                vx_store(cols[i][q + r], sum);
            }
        }
        vxf cost[PICK_MAX];
        for (int i = 0; i < n; i++) {
            vxf c = vx_load(cols[i][0]);
            for (int q = 1; q <= 2 * r; q++)
                c = vx_add(c, vx_load(cols[i][q]));
            cost[i] = c;
        }
        for (int s = 0; s < rows; s++) {
            const int y = y0 + s;
            /* The window of row s is cols[s .. s + 2 r]. */
            vxu px = mean_bytes(vxu_loadu(row_at(a, x + ax[0], y + ay[0])), vxu_loadu(row_at(b, x - bx[0], y - by[0])));
            vxf best = cost[0];
            for (int i = 1; i < n; i++) {
                const vxf c = vx_add(cost[i], vbias);
                const vxk k = vx_cmp(c, best, VX_CMP_LT);
                if (k) {
                    px = vxu_blend(k, px, mean_bytes(vxu_loadu(row_at(a, x + ax[i], y + ay[i])), vxu_loadu(row_at(b, x - bx[i], y - by[i]))));
                    best = vx_blend(k, best, c);
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

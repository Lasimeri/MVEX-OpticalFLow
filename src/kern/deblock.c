/* deblock.c: see deblock.h. Against vx.h: native MVEX on the card, the lane model on the host. Samples are whole numbers in F32, every step below exact, so the two agree bit for bit. */
#include "deblock.h"

#include <string.h>

#include "img.h"
#include "vxu.h"

#define ONE_PX 1 /* two blocks' vectors this far apart make a boundary: at this field's size, a pixel of the picture */

static const uint32_t ABS VX_ALIGNED = 0x7fffffffu, M_FF VX_ALIGNED = 0xffu, ALPHA VX_ALIGNED = 0xff000000u;
static const float K0 VX_ALIGNED = 0.0f, K255 VX_ALIGNED = 255.0f, K2 VX_ALIGNED = 2.0f, K3 VX_ALIGNED = 3.0f, K8 VX_ALIGNED = 8.0f, K9 VX_ALIGNED = 9.0f, K10 VX_ALIGNED = 10.0f, K16TH VX_ALIGNED = 0.0625f, KHALF VX_ALIGNED = 0.5f, K1 VX_ALIGNED = 1.0f;

static inline int boundary(const struct fg_vec *a, const struct fg_vec *b)
{
    const int dx = a->x - b->x, dy = a->y - b->y;
    return dx >= ONE_PX || -dx >= ONE_PX || dy >= ONE_PX || -dy >= ONE_PX;
}

static inline vxf clamp_to(vxf v, vxf lo, vxf hi) { return vx_min(vx_max(v, lo), hi); }
static inline vxf vx_add_k8(vxf v) { return vx_add(v, vx_bcast(&K8)); }

/* One channel across one edge, 16 lines at once: p2 p1 p0 | q0 q1 q2. Lanes outside k stay. H.265 8.7.2.5.7, with its decisions over the four lines of a segment taken here line by line, since nothing here is bound to segments: d, the two sides' curvature; delta, the correction,
 * refused when it is ten times what may be applied (a real edge); the second samples each side corrected when that side is flat enough. */
static inline void filter_channel(vxf p2, vxf *p1, vxf *p0, vxf *q0, vxf *q1, vxf q2, vxk k, const float *tc, const float *half_tc, const float *beta, const float *side)
{
    const vxf zero = vx_bcast(&K0), top = vx_bcast(&K255), two = vx_bcast(&K2);
    const vxf P1 = *p1, P0 = *p0, Q0 = *q0, Q1 = *q1;
    const vxf dp = vx_abs(vx_add(vx_sub(p2, vx_mul(P1, two)), P0), &ABS), dq = vx_abs(vx_add(vx_sub(q2, vx_mul(Q1, two)), Q0), &ABS);
    const vxk smooth = (vxk)(vx_cmp(vx_add(dp, dq), vx_bcast(beta), VX_CMP_LT) & k);
    if (!smooth)
        return;
    const vxf vtc = vx_bcast(tc), neg_tc = vx_sub(zero, vtc);
    const vxf delta = vx_round(vx_mul_s(vx_add_k8(vx_sub(vx_mul_s(vx_sub(Q0, P0), &K9), vx_mul_s(vx_sub(Q1, P1), &K3))), &K16TH), VX_RND_DOWN);
    const vxk apply = (vxk)(vx_cmp(vx_abs(delta, &ABS), vx_mul_s(vtc, &K10), VX_CMP_LT) & smooth);
    if (!apply)
        return;
    const vxf d = clamp_to(delta, neg_tc, vtc);
    *p0 = vx_blend(apply, P0, clamp_to(vx_add(P0, d), zero, top));
    *q0 = vx_blend(apply, Q0, clamp_to(vx_sub(Q0, d), zero, top));
    /* The second samples, half as far. */
    const vxf vh = vx_bcast(half_tc), neg_h = vx_sub(zero, vh), vside = vx_bcast(side);
    const vxk kp = (vxk)(vx_cmp(dp, vside, VX_CMP_LT) & apply), kq = (vxk)(vx_cmp(dq, vside, VX_CMP_LT) & apply);
    if (kp) {
        const vxf mp = vx_round(vx_mul_s(vx_add(vx_add(p2, P0), vx_bcast(&K1)), &KHALF), VX_RND_DOWN);
        const vxf dpp = clamp_to(vx_round(vx_mul_s(vx_add(vx_sub(mp, P1), d), &KHALF), VX_RND_DOWN), neg_h, vh);
        *p1 = vx_blend(kp, P1, clamp_to(vx_add(P1, dpp), zero, top));
    }
    if (kq) {
        const vxf mq = vx_round(vx_mul_s(vx_add(vx_add(q2, Q0), vx_bcast(&K1)), &KHALF), VX_RND_DOWN);
        const vxf dqq = clamp_to(vx_round(vx_mul_s(vx_sub(vx_sub(mq, Q1), d), &KHALF), VX_RND_DOWN), neg_h, vh);
        *q1 = vx_blend(kq, Q1, clamp_to(vx_add(Q1, dqq), zero, top));
    }
}

static inline vxf chan(vxu p, int shift) { return vxu_to_f(vxu_and_s(shift == 0 ? p : shift == 8 ? vxu_srl(p, 8) : vxu_srl(p, 16), &M_FF)); }
static inline vxu pack(vxf b, vxf g, vxf r) { return vxu_or(vxu_or(vxu_from_f(b), vxu_sll(vxu_from_f(g), 8)), vxu_or(vxu_sll(vxu_from_f(r), 16), vxu_bcast(&ALPHA))); }

/* All three channels of six registers of pixels across an edge. */
static inline void filter_edge(vxu p2, vxu *p1, vxu *p0, vxu *q0, vxu *q1, vxu q2, vxk k, const float *tc, const float *half_tc, const float *beta, const float *side)
{
    vxf ch_p1[3], ch_p0[3], ch_q0[3], ch_q1[3];
    for (int c = 0; c < 3; c++) {
        const int s = c * 8;
        ch_p1[c] = chan(*p1, s);
        ch_p0[c] = chan(*p0, s);
        ch_q0[c] = chan(*q0, s);
        ch_q1[c] = chan(*q1, s);
        filter_channel(chan(p2, s), &ch_p1[c], &ch_p0[c], &ch_q0[c], &ch_q1[c], chan(q2, s), k, tc, half_tc, beta, side);
    }
    *p1 = pack(ch_p1[0], ch_p1[1], ch_p1[2]);
    *p0 = pack(ch_p0[0], ch_p0[1], ch_p0[2]);
    *q0 = pack(ch_q0[0], ch_q0[1], ch_q0[2]);
    *q1 = pack(ch_q1[0], ch_q1[1], ch_q1[2]);
}

/* Across columns: the edge's eight lines lie in eight rows, so they are stood up in lanes, filtered, and laid back. */
void deblock_v_vx(const struct fg_field *mid, const struct bgra *pic, float tc, float beta, int band0, int band1)
{
    const float vtc VX_ALIGNED = tc, vh VX_ALIGNED = tc * 0.5f, vb VX_ALIGNED = beta, vs VX_ALIGNED = beta * 0.375f;
    uint32_t t[6][16] VX_ALIGNED;
    for (int by = band0; by < band1; by++) {
        const int y0 = by * 8, rows = y0 + 8 <= pic->h ? 8 : pic->h - y0;
        const struct fg_vec *row = mid->v + (size_t)(by < mid->h ? by : mid->h - 1) * (size_t)mid->w;
        for (int bx = 1; bx < mid->w; bx++) {
            const int e = bx * 8;
            if (e + 3 > pic->w || !boundary(&row[bx - 1], &row[bx]))
                continue;
            for (int r = 0; r < rows; r++) {
                const uint32_t *s = bgra_row(pic, y0 + r) + e - 3;
                for (int c = 0; c < 6; c++)
                    t[c][r] = s[c];
            }
            for (int c = 0; c < 6; c++)
                for (int r = rows; r < 16; r++)
                    t[c][r] = 0xff000000u;
            vxu p1 = vxu_load(t[1]), p0 = vxu_load(t[2]), q0 = vxu_load(t[3]), q1 = vxu_load(t[4]);
            filter_edge(vxu_load(t[0]), &p1, &p0, &q0, &q1, vxu_load(t[5]), (vxk)((1u << rows) - 1u), &vtc, &vh, &vb, &vs);
            vxu_store(t[1], p1);
            vxu_store(t[2], p0);
            vxu_store(t[3], q0);
            vxu_store(t[4], q1);
            for (int r = 0; r < rows; r++) {
                uint32_t *d = bgra_row(pic, y0 + r) + e - 2;
                d[0] = t[1][r];
                d[1] = t[2][r];
                d[2] = t[3][r];
                d[3] = t[4][r];
            }
        }
    }
    img_fence();
}

/* Across rows: the lines of an edge are the lanes of a row, two blocks to a register. */
void deblock_h_vx(const struct fg_field *mid, const struct bgra *pic, float tc, float beta, int band0, int band1)
{
    const float vtc VX_ALIGNED = tc, vh VX_ALIGNED = tc * 0.5f, vb VX_ALIGNED = beta, vs VX_ALIGNED = beta * 0.375f;
    for (int by = band0 ? band0 : 1; by < band1; by++) {
        const int y = by * 8;
        if (y + 3 > pic->h || by >= mid->h)
            continue;
        const struct fg_vec *up = mid->v + (size_t)(by - 1) * (size_t)mid->w, *dn = mid->v + (size_t)by * (size_t)mid->w;
        for (int bx = 0; bx < mid->w; bx += 2) {
            const unsigned k = (boundary(&up[bx], &dn[bx]) ? 0x00ffu : 0u) | (bx + 1 < mid->w && boundary(&up[bx + 1], &dn[bx + 1]) ? 0xff00u : 0u);
            if (!k)
                continue;
            const int x = bx * 8;
            vxu p1 = vxu_load(bgra_row(pic, y - 2) + x), p0 = vxu_load(bgra_row(pic, y - 1) + x), q0 = vxu_load(bgra_row(pic, y) + x), q1 = vxu_load(bgra_row(pic, y + 1) + x);
            filter_edge(vxu_load(bgra_row(pic, y - 3) + x), &p1, &p0, &q0, &q1, vxu_load(bgra_row(pic, y + 2) + x), (vxk)k, &vtc, &vh, &vb, &vs);
            vxu_store_nr(bgra_row(pic, y - 2) + x, p1);
            vxu_store_nr(bgra_row(pic, y - 1) + x, p0);
            vxu_store_nr(bgra_row(pic, y) + x, q0);
            vxu_store_nr(bgra_row(pic, y + 1) + x, q1);
        }
    }
    img_fence();
}

/* amdfi.c: see amdfi.h. Against vxu.h: native MVEX on the card, the lane model on the host, with x87 long double for the few scalars, the same on both, and an atomic maximum whose answer does not depend on who came first. */
#include "amdfi.h"

#include <math.h>
#include <string.h>

#include "img.h"
#include "vxu.h"

#define BLOCK 8
#define BIAS 32768u
#define PRIORITY_LOW_MAX 31u
#define PRIORITY_HIGH_MAX 1023u
#define PRIORITY_LOW_OFFSET 16
#define PRIORITY_HIGH_OFFSET 21
#define PRIMARY (1u << 31)
#define R(x) ((long double)(x))

static const uint32_t M_00FF00FF VX_ALIGNED = 0x00ff00ffu, M_7F VX_ALIGNED = 0x7f7f7f7fu, K_15 VX_ALIGNED = 15u, K_16 VX_ALIGNED = 16u, ROUND8 VX_ALIGNED = 0x00800080u, M_FF00FF00 VX_ALIGNED = 0xff00ff00u;
static const uint32_t ROUND_Q VX_ALIGNED = 128u;

void amdfi_init(struct amdfi *m, void *mem, int w, int h)
{
    m->w = (w + BLOCK - 1) / BLOCK;
    m->h = (h + BLOCK - 1) / BLOCK;
    m->x = mem;
    m->y = m->x + (size_t)m->w * (size_t)m->h;
}

void amdfi_clear(const struct amdfi *m) { memset(m->x, 0, 2u * (size_t)m->w * (size_t)m->h * sizeof(uint32_t)); }

static inline void atomic_max(uint32_t *p, uint32_t v)
{
    uint32_t cur = __atomic_load_n(p, __ATOMIC_RELAXED);
    while (v > cur && !__atomic_compare_exchange_n(p, &cur, v, 1, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
    }
}

/* len_factor of the shader: max(0, 512 - len) if len > 1, else 0. Over nine lanes. */
static inline vxf len_factor(vxf vx, vxf vy)
{
    static const float ONE VX_ALIGNED = 1.0f, K512 VX_ALIGNED = 512.0f, HALF VX_ALIGNED = 0.5f, THREE_HALVES VX_ALIGNED = 1.5f, Z VX_ALIGNED = 0.0f;
    const vxf one = vx_bcast(&ONE), zero = vx_bcast(&Z);
    const vxf len2 = vx_fma(vx, vx, vx_mul(vy, vy));
    const vxk big = vx_cmp(one, len2, VX_CMP_LT);
    const vxf len = vx_sqrt(vx_max(len2, one), &HALF, &THREE_HALVES);
    return vx_blend(big, zero, vx_max(zero, vx_sub(vx_bcast(&K512), len)));
}

void amdfi_field_vx(const struct amdfi *m, const struct fg_field *flow, const struct vt_plane *ya, const struct vt_plane *yb, int pic_w, int pic_h, int i0, int i1)
{
    static const float HALF VX_ALIGNED = 0.5f, THREE_HALVES VX_ALIGNED = 1.5f, ONE VX_ALIGNED = 1.0f, Z VX_ALIGNED = 0.0f, TINY VX_ALIGNED = 1e-6f, Q4 VX_ALIGNED = 16.0f;
    static const unsigned short NINE = 0x01ff;
    const long double rect = 0.05L * sqrtl(R(pic_w) * R(pic_w) + R(pic_h) * R(pic_h));
    for (int i = i0; i < i1; i++) {
        const int bx = i % flow->w, by = i / flow->w;
        int32_t xs[16] VX_ALIGNED, ys[16] VX_ALIGNED;
        int same = 1, n = 0;
        const struct fg_vec own = flow->v[i];
        for (int yy = -1; yy <= 1; yy++)
            for (int xx = -1; xx <= 1; xx++) {
                const int qx = bx + xx, qy = by + yy;
                /* The shader's Load off the texture reads zero. */
                const int on = qx >= 0 && qy >= 0 && qx < flow->w && qy < flow->h;
                const struct fg_vec v = on ? flow->v[qy * flow->w + qx] : (struct fg_vec){0, 0};
                xs[n] = v.x;
                ys[n] = v.y;
                same &= v.x == own.x && v.y == own.y;
                n++;
            }
        for (; n < 16; n++)
            xs[n] = ys[n] = 0;
        int qx4, qy4;
        if (same) {
            /* All nine agree: every weighted mean of them is that vector, if it is over a pixel long, and no motion if not. */
            const int l2 = (int)own.x * own.x + (int)own.y * own.y;
            if (l2 <= 1)
                continue;
            qx4 = own.x * 16;
            qy4 = own.y * 16;
        } else {
            const vxf vx = vxu_to_f(vxu_load((const uint32_t *)xs)), vy = vxu_to_f(vxu_load((const uint32_t *)ys));
            const vxf zero = vx_bcast(&Z);
            const vxf lf = vx_blend(NINE, zero, len_factor(vx, vy));
            const float sw1 VX_ALIGNED = vx_hsum(lf);
            if (sw1 <= 0.0f)
                continue;
            /* The mean, then each vector's agreement with it. */
            const vxf r1 = vx_rcp(vx_bcast(&sw1), &ONE);
            const float ax VX_ALIGNED = vx_hsum(vx_mul(vx, lf)), ay VX_ALIGNED = vx_hsum(vx_mul(vy, lf));
            const vxf mx = vx_mul(vx_bcast(&ax), r1), my = vx_mul(vx_bcast(&ay), r1);
            const vxf dot = vx_fma(mx, vx, vx_mul(my, vy));
            const vxk pos = vx_cmp(zero, dot, VX_CMP_LT);
            /* d ^ 1.25 = d x fourth root of d */
            const vxf d = vx_max(dot, vx_bcast(&TINY));
            const vxf root4 = vx_sqrt(vx_sqrt(d, &HALF, &THREE_HALVES), &HALF, &THREE_HALVES);
            const vxf w = vx_mul(vx_blend(pos, zero, vx_mul(d, root4)), lf);
            const float sw2 VX_ALIGNED = vx_hsum(w);
            if (sw2 <= 1e-7f)
                continue;
            const vxf r2 = vx_rcp(vx_bcast(&sw2), &ONE);
            const float bxv VX_ALIGNED = vx_hsum(vx_mul(vx, w)), byv VX_ALIGNED = vx_hsum(vx_mul(vy, w));
            int32_t q[16] VX_ALIGNED;
            vxu_store((uint32_t *)q, vxu_from_f(vx_round(vx_mul_s(vx_mul(vx_bcast(&bxv), r2), &Q4), VX_RND_NEAREST)));
            qx4 = q[0];
            vxu_store((uint32_t *)q, vxu_from_f(vx_round(vx_mul_s(vx_mul(vx_bcast(&byv), r2), &Q4), VX_RND_NEAREST)));
            qy4 = q[0];
        }
        /* computeOpticalFlowFieldMvs. */
        const long double fx = R(qx4) / 16.0L, fy = R(qy4) / 16.0L, vel = sqrtl(fx * fx + fy * fy);
        if (vel <= 1.0L)
            continue;
        const long double sat = vel / rect > 1.0L ? 1.0L : vel / rect;
        const uint32_t high = (uint32_t)(sat * R(PRIORITY_HIGH_MAX));
        if (!high)
            continue;
        const int cx = bx * BLOCK + BLOCK / 2, cy = by * BLOCK + BLOCK / 2;
        const int px = cx + (qx4 >> 4), py = cy + (qy4 >> 4);
        const int inside = px >= 0 && py >= 0 && px < pic_w && py < pic_h;
        uint32_t low = 0;
        if (inside) {
            const long double lp = 0.001L + R(plane_row(ya, py)[px]) / 16320.0L, lc = 0.001L + R(plane_row(yb, cy < pic_h ? cy : pic_h - 1)[cx < pic_w ? cx : pic_w - 1]) / 16320.0L;
            const long double mn = lp < lc ? lp : lc, mx_ = lp < lc ? lc : lp;
            low = (uint32_t)floorl(mn / mx_ * R(PRIORITY_LOW_MAX) + 0.5L);
        }
        const uint32_t pri = PRIMARY | (high << PRIORITY_HIGH_OFFSET) | (low << PRIORITY_LOW_OFFSET);
        const int hx = (qx4 + 1) >> 1, hy = (qy4 + 1) >> 1;
        const uint32_t wx = pri | (uint32_t)(hx + (int)BIAS), wy = pri | (uint32_t)(hy + (int)BIAS);
        /* GetBilinearSamplingData at the place half the motion away, in units of the field: all four, whatever their weights, as the shader writes them. */
        const int base_x = bx + (hx >> 7), base_y = by + (hy >> 7);
        for (int k = 0; k < 4; k++) {
            const int sx = base_x + (k & 1), sy = base_y + (k >> 1);
            if (sx < 0 || sy < 0 || sx >= m->w || sy >= m->h)
                continue;
            const size_t at = (size_t)sy * (size_t)m->w + (size_t)sx;
            atomic_max(&m->x[at], wx);
            atomic_max(&m->y[at], wy);
        }
    }
}

static inline int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
static inline int unpack(uint32_t e) { return e ? (int)(e & 0xffffu) - (int)BIAS : 0; }

/* One frame at sub-pixel positions in sixteenths, per lane: four words each, gathered, weighted in 16 bit fields of 32 bit lanes; weights sum to 256, so nothing carries across. Samples off the picture read its edge: the shader's dropping them and dividing by what is left comes to nearly this. */
static inline vxu fetch(const struct bgra *f, vxu px, vxu py, vxu max_x, vxu max_y, vxu zero, vxu stride)
{
    const vxu ix = vxu_sra(px, 4), iy = vxu_sra(py, 4);
    const vxu fx = vxu_and_s(px, &K_15), fy = vxu_and_s(py, &K_15);
    const vxu x0 = vxu_min_s(vxu_max_s(ix, zero), max_x), y0 = vxu_min_s(vxu_max_s(iy, zero), max_y);
    const vxu one = vxu_srl(vxu_bcast(&K_16), 4);
    const vxu x1 = vxu_min_s(vxu_max_s(vxu_add(ix, one), zero), max_x), y1 = vxu_min_s(vxu_max_s(vxu_add(iy, one), zero), max_y);
    const vxu row0 = vxu_mull(y0, stride), row1 = vxu_mull(y1, stride);
    const uint32_t *base = bgra_row(f, 0);
    const vxu p00 = vxu_gather(base, vxu_add(row0, x0)), p10 = vxu_gather(base, vxu_add(row0, x1)), p01 = vxu_gather(base, vxu_add(row1, x0)), p11 = vxu_gather(base, vxu_add(row1, x1));
    const vxu k16 = vxu_bcast(&K_16), gx = vxu_sub(k16, fx), gy = vxu_sub(k16, fy);
    const vxu w00 = vxu_mull(gx, gy), w10 = vxu_mull(fx, gy), w01 = vxu_mull(gx, fy), w11 = vxu_mull(fx, fy);
    vxu rb = vxu_mull(vxu_and_s(p00, &M_00FF00FF), w00), ag = vxu_mull(vxu_and_s(vxu_srl(p00, 8), &M_00FF00FF), w00);
    rb = vxu_add(rb, vxu_mull(vxu_and_s(p10, &M_00FF00FF), w10));
    ag = vxu_add(ag, vxu_mull(vxu_and_s(vxu_srl(p10, 8), &M_00FF00FF), w10));
    rb = vxu_add(rb, vxu_mull(vxu_and_s(p01, &M_00FF00FF), w01));
    ag = vxu_add(ag, vxu_mull(vxu_and_s(vxu_srl(p01, 8), &M_00FF00FF), w01));
    rb = vxu_add(rb, vxu_mull(vxu_and_s(p11, &M_00FF00FF), w11));
    ag = vxu_add(ag, vxu_mull(vxu_and_s(vxu_srl(p11, 8), &M_00FF00FF), w11));
    return vxu_or(vxu_and_s(vxu_srl(vxu_add_s(rb, &ROUND8), 8), &M_00FF00FF), vxu_and_s(vxu_add_s(ag, &ROUND8), &M_FF00FF00));
}

#if defined(__KNC__)
static inline vxu perm_u(vxu v, vxidx ix) { return __builtin_ia32_knc_vpermd_zzz(VX_UI, VX_ALL, ix, v); }
#else
static inline vxu perm_u(vxu v, vxidx ix) { vxu r; VX_LANE_LOOP r.l[i] = v.l[ix.l[i] & 15]; return r; }
#endif

static inline vxu mean_px(vxu a, vxu b) { return vxu_sub(vxu_or(a, b), vxu_and_s(vxu_srl(vxu_xor(a, b), 1), &M_7F)); }

/* Which lanes' four samples miss the picture altogether: such a frame has nothing to give there. */
static inline unsigned wholly_off(vxu pos, int top)
{
    int32_t v[16] VX_ALIGNED;
    vxu_store((uint32_t *)v, vxu_sra(pos, 4));
    unsigned k = 0;
    for (int i = 0; i < 16; i++)
        k |= (unsigned)(v[i] < -1 || v[i] > top) << i;
    return k;
}

void amdfi_picture_vx(const struct amdfi *m, const struct bgra *a, const struct bgra *b, const struct bgra *out, int t0, int t1)
{
    /* Across a row: which of four columns of field places lie either side of each lane, and how far along; the place's centre lies at 8 c + 4. */
    static const int32_t LEFT[16] VX_ALIGNED = {0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2};
    static const int32_t RIGHT[16] VX_ALIGNED = {1, 1, 1, 1, 2, 2, 2, 2, 2, 2, 2, 2, 3, 3, 3, 3};
    static const uint32_t WX[16] VX_ALIGNED = {9, 11, 13, 15, 1, 3, 5, 7, 9, 11, 13, 15, 1, 3, 5, 7};
    static const uint32_t LANE_Q4[16] VX_ALIGNED = {0, 16, 32, 48, 64, 80, 96, 112, 128, 144, 160, 176, 192, 208, 224, 240};
    const int tw = (out->w + 15) / 16;
    const uint32_t Z VX_ALIGNED = 0u, MX VX_ALIGNED = (uint32_t)(out->w - 1), MY VX_ALIGNED = (uint32_t)(out->h - 1), ST VX_ALIGNED = (uint32_t)out->stride;
    const vxu zero = vxu_bcast(&Z), max_x = vxu_bcast(&MX), max_y = vxu_bcast(&MY), stride = vxu_bcast(&ST), wx = vxu_load(WX), k16 = vxu_bcast(&K_16), lane_q4 = vxu_load(LANE_Q4);
    const vxidx il = vx_idx(LEFT), ir = vx_idx(RIGHT);
    for (int t = t0; t < t1; t++) {
        const int tx = t % tw, ty = t / tw, x = tx * 16, y0 = ty * 16, y1 = y0 + 16 < out->h ? y0 + 16 : out->h;
        const int c0 = tx * 2 - 1, r0 = ty * 2 - 1;
        /* Nothing landed anywhere near: no motion here. */
        uint32_t any = 0;
        for (int r = 0; r < 4; r++)
            for (int c = 0; c < 4; c++) {
                const size_t at = (size_t)clampi(r0 + r, 0, m->h - 1) * (size_t)m->w + (size_t)clampi(c0 + c, 0, m->w - 1);
                any |= m->x[at] | m->y[at];
            }
        if (!any) {
            for (int y = y0; y < y1; y++)
                vxu_store_nr(bgra_row(out, y) + x, mean_px(vxu_load(bgra_row(a, y) + x), vxu_load(bgra_row(b, y) + x)));
            continue;
        }
        const uint32_t XQ VX_ALIGNED = (uint32_t)(x * 16);
        const vxu base_x = vxu_add_s(lane_q4, &XQ);
        for (int y = y0; y < y1; y++) {
            /* SampleOpticalFlowMotionVectorField: up the rows once for the row, then along the lanes by table. */
            const int fy2 = 2 * y - 7, by = fy2 >> 4, wy = fy2 & 15;
            int32_t col_x[16] VX_ALIGNED, col_y[16] VX_ALIGNED;
            for (int c = 0; c < 4; c++) {
                const int cc = clampi(c0 + c, 0, m->w - 1);
                const size_t up = (size_t)clampi(by, 0, m->h - 1) * (size_t)m->w + (size_t)cc, dn = (size_t)clampi(by + 1, 0, m->h - 1) * (size_t)m->w + (size_t)cc;
                col_x[c] = (16 - wy) * unpack(m->x[up]) + wy * unpack(m->x[dn]);
                col_y[c] = (16 - wy) * unpack(m->y[up]) + wy * unpack(m->y[dn]);
            }
            for (int c = 4; c < 16; c++)
                col_x[c] = col_y[c] = 0;
            /* In 16 x 16 ths of a sixteenth: vpermd over a register whose first four lanes are the columns. */
            const vxu cx = vxu_load((const uint32_t *)col_x), cy = vxu_load((const uint32_t *)col_y);
            const vxu rw = vxu_sub(k16, wx);
            const vxu mxq = vxu_add(vxu_mull(perm_u(cx, il), rw), vxu_mull(perm_u(cx, ir), wx));
            const vxu myq =vxu_add(vxu_mull(perm_u(cy, il), rw), vxu_mull(perm_u(cy, ir), wx));
            const vxu mx4 = vxu_sra(vxu_add_s(mxq, &ROUND_Q), 8), my4 = vxu_sra(vxu_add_s(myq, &ROUND_Q), 8);
            const uint32_t YQ VX_ALIGNED = (uint32_t)(y * 16);
            const vxu ypos = vxu_bcast(&YQ);
            /* The older frame at + the vector, the newer at -. */
            const vxu ax = vxu_add(base_x, mx4), ay = vxu_add(ypos, my4), bx = vxu_sub(base_x, mx4), by2 = vxu_sub(ypos, my4);
            const unsigned off_a = wholly_off(ax, out->w - 1) | wholly_off(ay, out->h - 1), off_b = wholly_off(bx, out->w - 1) | wholly_off(by2, out->h - 1);
            const vxu pa = fetch(a, ax, ay, max_x, max_y, zero, stride), pb = fetch(b, bx, by2, max_x, max_y, zero, stride);
            /* The shader's ofT: their mean; the one frame that has the place. */
            vxu px = mean_px(pa, pb);
            px = vxu_blend((vxk)(off_a & ~off_b), px, pb);
            px = vxu_blend((vxk)(off_b & ~off_a), px, pa);
            vxu_store_nr(bgra_row(out, y) + x, px);
        }
    }
    img_fence();
}

/* computeOpticalFlowVectorField alone: the 3 x 3 weighted smoothing of a field into another of the same size, to whole pixels, for a picture that is not AMD's. A vector of a pixel or less stays as it is here: AMD's own path has no use for those, this one does. */
void amdfi_smooth_vx(const struct fg_field *in, const struct fg_field *out, int i0, int i1)
{
    static const float HALF VX_ALIGNED = 0.5f, THREE_HALVES VX_ALIGNED = 1.5f, ONE VX_ALIGNED = 1.0f, Z VX_ALIGNED = 0.0f, TINY VX_ALIGNED = 1e-6f;
    static const unsigned short NINE = 0x01ff;
    for (int i = i0; i < i1; i++) {
        const int bx = i % in->w, by = i / in->w;
        int32_t xs[16] VX_ALIGNED, ys[16] VX_ALIGNED;
        int same = 1, n = 0;
        const struct fg_vec own = in->v[i];
        out->v[i] = own;
        for (int yy = -1; yy <= 1; yy++)
            for (int xx = -1; xx <= 1; xx++) {
                const int qx = bx + xx, qy = by + yy;
                const int on = qx >= 0 && qy >= 0 && qx < in->w && qy < in->h;
                const struct fg_vec v = on ? in->v[qy * in->w + qx] : (struct fg_vec){0, 0};
                xs[n] = v.x;
                ys[n] = v.y;
                same &= !on || (v.x == own.x && v.y == own.y);
                n++;
            }
        if (same)
            continue;
        for (; n < 16; n++)
            xs[n] = ys[n] = 0;
        const vxf vx = vxu_to_f(vxu_load((const uint32_t *)xs)), vy = vxu_to_f(vxu_load((const uint32_t *)ys));
        const vxf zero = vx_bcast(&Z);
        const vxf lf = vx_blend(NINE, zero, len_factor(vx, vy));
        const float sw1 VX_ALIGNED = vx_hsum(lf);
        if (sw1 <= 0.0f)
            continue;
        const vxf r1 = vx_rcp(vx_bcast(&sw1), &ONE);
        const float ax VX_ALIGNED = vx_hsum(vx_mul(vx, lf)), ay VX_ALIGNED = vx_hsum(vx_mul(vy, lf));
        const vxf dot = vx_fma(vx_mul(vx_bcast(&ax), r1), vx, vx_mul(vx_mul(vx_bcast(&ay), r1), vy));
        const vxk pos = vx_cmp(zero, dot, VX_CMP_LT);
        const vxf d = vx_max(dot, vx_bcast(&TINY));
        const vxf root4 = vx_sqrt(vx_sqrt(d, &HALF, &THREE_HALVES), &HALF, &THREE_HALVES);
        const vxf w = vx_mul(vx_blend(pos, zero, vx_mul(d, root4)), lf);
        const float sw2 VX_ALIGNED = vx_hsum(w);
        if (sw2 <= 1e-7f)
            continue;
        const vxf r2 = vx_rcp(vx_bcast(&sw2), &ONE);
        const float bxv VX_ALIGNED = vx_hsum(vx_mul(vx, w)), byv VX_ALIGNED = vx_hsum(vx_mul(vy, w));
        int32_t q[16] VX_ALIGNED;
        vxu_store((uint32_t *)q, vxu_from_f(vx_round(vx_mul(vx_bcast(&bxv), r2), VX_RND_NEAREST)));
        out->v[i].x = (int16_t)q[0];
        vxu_store((uint32_t *)q, vxu_from_f(vx_round(vx_mul(vx_bcast(&byv), r2), VX_RND_NEAREST)));
        out->v[i].y = (int16_t)q[0];
    }
}

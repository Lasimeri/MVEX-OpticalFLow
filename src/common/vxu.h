/* vxu.h: the integer half of the MVEX vocabulary, the partner of vx.h and in its form: one name, two bodies, native on the card, a lane model on the host. Packed BGRA pixels are handled here as what they are, 16 pixels to a
 * register, by byte-wise arithmetic on 32-bit lanes (the card has no byte instructions). */
#ifndef MF_VXU_H
#define MF_VXU_H

#include "vx.h"

#if defined(__KNC__)

typedef __v16si vxu;

static inline vxu vxu_zero(void) { return (__v16si)_mm512_setzero_epi32(); }
static inline vxu vxu_load(const uint32_t *p) { return (__v16si)_mm512_load_epi32(p); }
/* 16 lanes from any 4 B aligned address: the unpack pair. */
static inline vxu vxu_loadu(const uint32_t *p)
{
    const __v16si v = __builtin_ia32_knc_vloadunpackld_zm(VX_UI, VX_ALL, p, 0);
    return __builtin_ia32_knc_vloadunpackhd_zm(v, VX_ALL, (const char *)p + 64, 0);
}
/* A whole line with no read-for-ownership; a fence (img_fence) before another thread or the DMA engine reads it. */
static inline void vxu_store_nr(uint32_t *p, vxu v) { __builtin_ia32_knc_vmovnrngoaps_mz(VX_ALL, (float *)p, (__v16sf)v, 0); }
static inline vxu vxu_bcast(const uint32_t *p) { return (__v16si)__builtin_ia32_knc_vbroadcastss_zm(VX_U, VX_ALL, (const float *)p, 0); }
static inline vxu vxu_and(vxu a, vxu b) { return __builtin_ia32_knc_vpandd_zzz(VX_UI, VX_ALL, a, b, 0); }
static inline vxu vxu_or(vxu a, vxu b) { return __builtin_ia32_knc_vpord_zzz(VX_UI, VX_ALL, a, b, 0); }
static inline vxu vxu_xor(vxu a, vxu b) { return __builtin_ia32_knc_vpxord_zzz(VX_UI, VX_ALL, a, b, 0); }
static inline vxu vxu_add(vxu a, vxu b) { return __builtin_ia32_knc_vpaddd_zzz(VX_UI, VX_ALL, a, b, 0); }
static inline vxu vxu_sub(vxu a, vxu b) { return __builtin_ia32_knc_vpsubd_zzz(VX_UI, VX_ALL, a, b, 0); }
static inline vxu vxu_mull(vxu a, vxu b) { return __builtin_ia32_knc_vpmulld_zzz(VX_UI, VX_ALL, a, b, 0); }
/* With a {1to16} memory operand: no broadcast register. */
static inline vxu vxu_and_s(vxu a, const uint32_t *s) { return __builtin_ia32_knc_vpandd_zzm(VX_UI, VX_ALL, a, s, VX_SSS_1TO16); }
static inline vxu vxu_add_s(vxu a, const uint32_t *s) { return __builtin_ia32_knc_vpaddd_zzm(VX_UI, VX_ALL, a, s, VX_SSS_1TO16); }
#define vxu_srl(v, n) (__builtin_ia32_knc_vpsrld_zzi(VX_UI, VX_ALL, (v), (n), 0))
#define vxu_sll(v, n) (__builtin_ia32_knc_vpslld_zzi(VX_UI, VX_ALL, (v), (n), 0))
/* Lanes of k from b, the rest from a. */
static inline vxu vxu_blend(vxk k, vxu a, vxu b) { return (__v16si)__builtin_ia32_knc_vblendmps_zzz(k, (__v16sf)a, (__v16sf)b, 0); }
/* Whole numbers to F32 lanes, exact below 2^24. */
static inline vxf vxu_to_f(vxu v) { return (vxf)__builtin_ia32_knc_vcvtfxpntdq2ps_zzi(VX_U, VX_ALL, v, 0, 0); }
/* Whole-valued F32 lanes to numbers. */
static inline vxu vxu_from_f(vxf v) { return __builtin_ia32_knc_vcvtfxpntps2dq_zzi(VX_UI, VX_ALL, (__v16sf)v, 0, 0); }

#else /* the host model */

typedef struct { uint32_t l[16]; } vxu;

static inline vxu vxu_zero(void) { vxu r; memset(r.l, 0, sizeof r.l); return r; }
static inline vxu vxu_load(const uint32_t *p) { vxu r; memcpy(r.l, p, sizeof r.l); return r; }
static inline vxu vxu_loadu(const uint32_t *p) { return vxu_load(p); }
static inline void vxu_store_nr(uint32_t *p, vxu v) { memcpy(p, v.l, sizeof v.l); }
static inline vxu vxu_bcast(const uint32_t *p) { vxu r; VX_LANE_LOOP r.l[i] = *p; return r; }
static inline vxu vxu_and(vxu a, vxu b) { VX_LANE_LOOP a.l[i] &= b.l[i]; return a; }
static inline vxu vxu_or(vxu a, vxu b) { VX_LANE_LOOP a.l[i] |= b.l[i]; return a; }
static inline vxu vxu_xor(vxu a, vxu b) { VX_LANE_LOOP a.l[i] ^= b.l[i]; return a; }
static inline vxu vxu_add(vxu a, vxu b) { VX_LANE_LOOP a.l[i] += b.l[i]; return a; }
static inline vxu vxu_sub(vxu a, vxu b) { VX_LANE_LOOP a.l[i] -= b.l[i]; return a; }
static inline vxu vxu_mull(vxu a, vxu b) { VX_LANE_LOOP a.l[i] *= b.l[i]; return a; }
static inline vxu vxu_and_s(vxu a, const uint32_t *s) { VX_LANE_LOOP a.l[i] &= *s; return a; }
static inline vxu vxu_add_s(vxu a, const uint32_t *s) { VX_LANE_LOOP a.l[i] += *s; return a; }
static inline vxu vxu_srl_model(vxu a, int n) { VX_LANE_LOOP a.l[i] >>= n; return a; }
static inline vxu vxu_sll_model(vxu a, int n) { VX_LANE_LOOP a.l[i] <<= n; return a; }
#define vxu_srl(v, n) vxu_srl_model((v), (n))
#define vxu_sll(v, n) vxu_sll_model((v), (n))
static inline vxu vxu_blend(vxk k, vxu a, vxu b) { VX_LANE_LOOP if ((k >> i) & 1) a.l[i] = b.l[i]; return a; }
static inline vxf vxu_to_f(vxu v) { vxf r; VX_LANE_LOOP r.l[i] = (float)(int32_t)v.l[i]; return r; }
static inline vxu vxu_from_f(vxf v) { vxu r; VX_LANE_LOOP r.l[i] = (uint32_t)(int32_t)lrintf(v.l[i]); return r; }

#endif

#endif

/* flow.h: one monitor's frames through AMD's flow, in the order the port runs it (Intel-Phi-VisionTrack track.c, tk_generate_make and fg_build_more, taken out of the tracker): frames land, luma, pyramid; then between the two newest:
 * histograms, coarsest level first search, filter, upscale, the mid vectors, the picture. The same code runs on the card under a team of threads and on the host on one; the answer does not depend on how many threads. */
#ifndef MF_FLOW_H
#define MF_FLOW_H

#include <stddef.h>
#include <stdint.h>

#include "bgra.h"
#include "framegen.h"
#include "fsr3.h"

enum flow_stage { FLOW_LAND, FLOW_PYR, FLOW_HIST, FLOW_SEARCH, FLOW_FILTER, FLOW_UP, FLOW_MID, FLOW_BLEND, FLOW_FSR, FLOW_DEBLOCK, FLOW_STAGES };
/* FUSED, PLAIN: the port's field at half the picture's size, windows over 16 x 16 tiles, the two forms of its picture. PIXEL: the field at the picture's own size, a vector per pixel, whole-pixel samples. */
enum flow_form { FLOW_FUSED, FLOW_PLAIN, FLOW_PIXEL };

typedef void (*flow_fn)(void *arg, int tid, int n);
typedef void (*flow_run_fn)(void *ctx, flow_fn fn, void *arg);

struct flow {
    int w, h, levels, form;
    struct bgra frame[2], out;
    struct vt_plane y, pyr[2][FG_MAX_LEVELS];
    struct vt_plane chan[2][3], chan_out[3];
    struct fg_job fg, fgc[3];
    flow_run_fn run;
    void *run_ctx;
    int newest;             /* the frame that lands next, once flow_land has run: the newest */
    uint64_t frames, hist_frame;
    int have_prev;
    uint64_t cycles[FLOW_STAGES];
    uint32_t moved;         /* blocks of the last field with a nonzero mid vector */
    /* The round's order. */
    int op, level, tiles;
    int pick_r, pick_h, hist_level, pick_phase;
    /* What follows frame generation, FLOW_PIXEL: FSR on the first team, native, no sharpening, luma; deblocking on the second. */
    flow_run_fn run2;
    void *run2_ctx;
    struct fsr *fsr;
    int use_fsr, use_deblock;
    float db_tc, db_beta;
    struct bgra pic;        /* the generated picture after FSR */
    const struct bgra *final; /* where the generated picture ended, after the last stage */
    struct vt_plane ym, ym_prev; /* its luma, and the last one's */
    int ym_valid;           /* the last frame made a generated picture too */
    uint64_t fsr_cycles[FSR_STAGES + 1], fsr_runs;
    int fine_stride;        /* FLOW_PIXEL: cells of the finest vectors, 8 (the field itself), 4 or 2 pixels */
    struct fg_field fine;
    struct fg_vec *fine_mem;
    float pick_bias;
    int next __attribute__((aligned(64)));
};

int flow_levels(int w, int h, int form);
size_t flow_bytes(int w, int h, int form);
/* mem: flow_bytes(), 64 B aligned. 0, or -1. */
int flow_init(struct flow *f, void *mem, int w, int h, int form, flow_run_fn run, void *ctx);

/* Which frame the next landing goes in, for the caller's DMA or copy. */
static inline const struct bgra *flow_next_frame(const struct flow *f) { return &f->frame[!f->newest]; }

/* The next frame has landed in flow_next_frame(): it becomes the newest; padding, luma and pyramid. */
void flow_land(struct flow *f);

/* Between the two newest frames: 1 with the picture in f->out; 0 at a scene change, no picture; -1 when there are not yet two frames. */
int flow_make(struct flow *f);

/* After flow_make made a picture: FSR, then deblocking on the second team (run2 NULL: the first). The picture ends in *f->final. A frame that made none says so, and the next picture starts FSR afresh. */
void flow_finish_generated(struct flow *f);
void flow_no_generated(struct flow *f);

#endif

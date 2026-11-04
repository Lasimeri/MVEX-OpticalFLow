/* flow.c: see flow.h. */
#include "flow.h"

#include <string.h>

#include "img.h"
#include "vt_util.h"

#define CHUNK 16 /* blocks or tiles taken at a time */

enum { OP_LAND, OP_REDUCE, OP_HIST_A, OP_HIST_B, OP_SEARCH, OP_FILTER, OP_UP, OP_MID, OP_BLEND, OP_BLEND_PLAIN, OP_MERGE, OP_PICK };

static inline int row_of(int h, int tid, int n) { return (int)((int64_t)h * tid / n); }

int flow_levels(int w, int h, int form)
{
    int ww = form == FLOW_PIXEL ? w : (w + 1) / 2, hh = form == FLOW_PIXEL ? h : (h + 1) / 2, count = 0;
    for (int i = 0; i < FG_MAX_LEVELS && ww >= 16 && hh >= 16; i++) {
        count++;
        ww = (ww + 1) / 2;
        hh = (hh + 1) / 2;
    }
    return count;
}

size_t flow_bytes(int w, int h, int form)
{
    const int levels = flow_levels(w, h, form);
    size_t n = 3u * bgra_bytes(w, h) + plane_bytes(w, h);
    int ww = form == FLOW_PIXEL ? w : (w + 1) / 2, hh = form == FLOW_PIXEL ? h : (h + 1) / 2;
    n += fg_bytes(ww, hh, levels);
    for (int i = 0; i < levels; i++) {
        n += 2u * plane_bytes(ww, hh);
        ww = (ww + 1) / 2;
        hh = (hh + 1) / 2;
    }
    if (form == FLOW_PLAIN)
        n += 9u * plane_bytes(w, h);
    return n + 4096u;
}

static void *carve(char **m, size_t bytes)
{
    void *p = *m;
    *m += (bytes + 63u) & ~(size_t)63u;
    return p;
}

int flow_init(struct flow *f, void *mem, int w, int h, int form, flow_run_fn run, void *ctx)
{
    if (bgra_init())
        return -1;
    memset(f, 0, sizeof *f);
    char *m = mem;
    f->w = w;
    f->h = h;
    f->form = form;
    f->run = run;
    f->run_ctx = ctx;
    f->levels = flow_levels(w, h, form);
    f->hist_frame = ~0ull;
    for (int i = 0; i < 2; i++)
        f->frame[i] = bgra_at(carve(&m, bgra_bytes(w, h)), w, h);
    f->out = bgra_at(carve(&m, bgra_bytes(w, h)), w, h);
    f->y = plane_at(carve(&m, plane_bytes(w, h)), w, h);
    int ww = form == FLOW_PIXEL ? w : (w + 1) / 2, hh = form == FLOW_PIXEL ? h : (h + 1) / 2;
    const size_t fgb = fg_bytes(ww, hh, f->levels);
    fg_init(&f->fg, carve(&m, fgb), ww, hh, f->levels);
    for (int i = 0; i < f->levels; i++) {
        for (int s = 0; s < 2; s++)
            f->pyr[s][i] = plane_at(carve(&m, plane_bytes(ww, hh)), ww, hh);
        ww = (ww + 1) / 2;
        hh = (hh + 1) / 2;
    }
    if (form == FLOW_PLAIN)
        for (int c = 0; c < 3; c++) {
            for (int s = 0; s < 2; s++)
                f->chan[s][c] = plane_at(carve(&m, plane_bytes(w, h)), w, h);
            f->chan_out[c] = plane_at(carve(&m, plane_bytes(w, h)), w, h);
        }
    f->tiles = ((w + 15) / 16) * ((h + 15) / 16);
    f->pick_r = 2;
    f->pick_h = 2;
    f->pick_bias = 0.0f;
    /* The prediction from the level above is always even: under a loose early-out, faint detail moving an odd distance would keep it and blur. A quarter of a grey level a pixel: measured 1.5 ms dearer than the port's 2 on real footage. */
    if (form == FLOW_PIXEL)
        f->fg.enough = 1024.0f;
    f->hist_level = form == FLOW_PIXEL ? 2 : 1;
    return 0;
}

static void flow_main(void *arg, int tid, int n)
{
    struct flow *f = arg;
    const int op = f->op;
    switch (op) {
    case OP_LAND: {
        const struct bgra *s = &f->frame[f->newest];
        const int y0 = row_of(f->h, tid, n), y1 = row_of(f->h, tid + 1, n);
        bgra_pad_rows(s, y0, y1);
        const struct vt_plane *y = f->form == FLOW_PIXEL ? &f->pyr[f->newest][0] : &f->y;
        bgra_luma_vx(y, s, y0, y1);
        img_pad_rows(y, y0, y1);
        if (f->form == FLOW_PLAIN) {
            const struct vt_plane *c = f->chan[f->newest];
            bgra_split_vx(c, s, y0, y1);
            for (int k = 0; k < 3; k++)
                img_pad_rows(&c[k], y0, y1);
        }
        img_fence();
        break;
    }
    case OP_REDUCE: {
        const struct vt_plane *d = &f->pyr[f->newest][f->level], *s = f->level ? &f->pyr[f->newest][f->level - 1] : &f->y;
        const int y0 = row_of(d->h, tid, n), y1 = row_of(d->h, tid + 1, n);
        img_reduce_vx(d, s, y0, y1);
        img_pad_rows(d, y0, y1);
        img_fence();
        break;
    }
    case OP_HIST_A:
    case OP_HIST_B:
        for (;;) {
            const int s = __atomic_fetch_add(&f->next, 1, __ATOMIC_RELAXED);
            if (s >= FG_SECTIONS)
                break;
            if (op == OP_HIST_A)
                fg_hist_section(&f->fg.a[f->hist_level], f->fg.hist_a, s);
            else
                fg_hist_section(&f->fg.b[f->hist_level], f->fg.hist_b, s);
        }
        break;
    case OP_SEARCH:
    case OP_FILTER:
    case OP_UP:
    case OP_MID: {
        const struct fg_field *field = op == OP_UP ? &f->fg.up[f->level - 1] : op == OP_MID ? &f->fg.mid : &f->fg.raw[f->level];
        const int count = field->w * field->h;
        for (;;) {
            const int i = __atomic_fetch_add(&f->next, CHUNK, __ATOMIC_RELAXED);
            if (i >= count)
                break;
            const int e = i + CHUNK < count ? i + CHUNK : count;
            if (op == OP_SEARCH)
                fg_search(&f->fg, f->level, i, e);
            else if (op == OP_FILTER)
                fg_filter(&f->fg, f->level, i, e);
            else if (op == OP_UP)
                fg_upscale(&f->fg, f->level, i, e);
            else
                fg_mid_vectors(&f->fg, i, e);
        }
        break;
    }
    case OP_BLEND: {
        const struct bgra *a = &f->frame[!f->newest], *b = &f->frame[f->newest];
        for (;;) {
            const int i = __atomic_fetch_add(&f->next, CHUNK, __ATOMIC_RELAXED);
            if (i >= f->tiles)
                break;
            bgra_blend_vx(&f->fg.mid, a, b, &f->out, i, i + CHUNK < f->tiles ? i + CHUNK : f->tiles);
        }
        break;
    }
    case OP_BLEND_PLAIN:
        for (;;) {
            const int per = (f->tiles + CHUNK - 1) / CHUNK, i = __atomic_fetch_add(&f->next, 1, __ATOMIC_RELAXED);
            if (i >= 3 * per)
                break;
            const int t = (i % per) * CHUNK;
            fg_blend(&f->fgc[i / per], t, t + CHUNK < f->tiles ? t + CHUNK : f->tiles);
        }
        img_fence();
        break;
    case OP_PICK:
        for (;;) {
            const int i = __atomic_fetch_add(&f->next, CHUNK, __ATOMIC_RELAXED);
            if (i >= f->tiles)
                break;
            bgra_pick_vx(&f->fg.mid, &f->fg.fil[1], &f->pyr[!f->newest][0], &f->pyr[f->newest][0], &f->frame[!f->newest], &f->frame[f->newest], &f->out, f->pick_r, f->pick_h, f->pick_bias, i, i + CHUNK < f->tiles ? i + CHUNK : f->tiles);
        }
        break;
    case OP_MERGE:
        bgra_merge_vx(&f->out, f->chan_out, row_of(f->h, tid, n), row_of(f->h, tid + 1, n));
        break;
    default:
        break;
    }
}

static void round_of(struct flow *f, int op, int level)
{
    f->op = op;
    f->level = level;
    __atomic_store_n(&f->next, 0, __ATOMIC_RELAXED);
    f->run(f->run_ctx, flow_main, f);
}

void flow_land(struct flow *f)
{
    uint64_t mark = vt_tsc();
    f->newest = !f->newest;
    f->frames++;
    memset(f->cycles, 0, sizeof f->cycles);
    round_of(f, OP_LAND, 0);
    bgra_pad_tb(&f->frame[f->newest]);
    img_pad_tb(f->form == FLOW_PIXEL ? &f->pyr[f->newest][0] : &f->y);
    if (f->form == FLOW_PLAIN)
        for (int c = 0; c < 3; c++)
            img_pad_tb(&f->chan[f->newest][c]);
    uint64_t now = vt_tsc();
    f->cycles[FLOW_LAND] = now - mark;
    mark = now;
    for (int L = f->form == FLOW_PIXEL ? 1 : 0; L < f->levels; L++) {
        round_of(f, OP_REDUCE, L);
        img_pad_tb(&f->pyr[f->newest][L]);
    }
    f->cycles[FLOW_PYR] = vt_tsc() - mark;
}

int flow_make(struct flow *f)
{
    if (f->frames < 2)
        return -1;
    const int older = !f->newest, newer = f->newest;
    uint64_t mark = vt_tsc();
#define LAP(stage)                       \
    do {                                 \
        const uint64_t lap_ = vt_tsc();  \
        f->cycles[stage] += lap_ - mark; \
        mark = lap_;                     \
    } while (0)
    struct fg_job *j = &f->fg;
    for (int i = 0; i < f->levels; i++) {
        j->a[i] = f->pyr[older][i];
        j->b[i] = f->pyr[newer][i];
    }
    /* The older frame's histogram is the last one's newer, if the last frame made one. */
    if (f->hist_frame == f->frames - 1) {
        uint32_t *s = j->hist_a;
        j->hist_a = j->hist_b;
        j->hist_b = s;
    } else {
        round_of(f, OP_HIST_A, 0);
    }
    round_of(f, OP_HIST_B, 0);
    f->hist_frame = f->frames;
    j->scene_changed = fg_scene_verdict(j);
    LAP(FLOW_HIST);
    if (j->scene_changed)
        return 0;
    for (int L = f->levels - 1; L >= 0; L--) {
        round_of(f, OP_SEARCH, L);
        LAP(FLOW_SEARCH);
        round_of(f, OP_FILTER, L);
        LAP(FLOW_FILTER);
        if (L > 0)
            round_of(f, OP_UP, L);
        LAP(FLOW_UP);
    }
    round_of(f, OP_MID, 0);
    uint32_t moved = 0;
    const int blocks = j->mid.w * j->mid.h;
    for (int i = 0; i < blocks; i++)
        moved += (j->mid.v[i].x | j->mid.v[i].y) != 0;
    f->moved = moved;
    LAP(FLOW_MID);
    if (f->form == FLOW_PIXEL) {
        round_of(f, OP_PICK, 0);
    } else if (f->form == FLOW_PLAIN) {
        for (int c = 0; c < 3; c++) {
            f->fgc[c] = *j;
            f->fgc[c].a0 = f->chan[older][c];
            f->fgc[c].b0 = f->chan[newer][c];
            f->fgc[c].out = f->chan_out[c];
        }
        round_of(f, OP_BLEND_PLAIN, 0);
        round_of(f, OP_MERGE, 0);
    } else {
        round_of(f, OP_BLEND, 0);
    }
    LAP(FLOW_BLEND);
#undef LAP
    return 1;
}

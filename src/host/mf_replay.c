/* mf_replay: the gate. Frames go through the real path (the part, the card's DMA, mvexflowd, back), and each generated picture is held against:
 *   --ref    the same kernels on the host's lane model, byte for byte over every visible pixel (a control flips one byte to prove the comparison can fail);
 *   truth    a real frame left out between the two given: dB against it for the generated picture, for the mean of the two frames and for the older frame again, over the whole picture and over tiles that moved.
 *
 * Frames come from a synthetic scene (exact truth at every half step) or a raw BGRA file. No window, nothing on the monitor. */
#define _GNU_SOURCE
#include <fcntl.h>
#include <pthread.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "flow.h"
#include "mf_link.h"

static int W = 1366, H = 768;

/* ---- scenes ---- */

#define TEX_W 4096
#define TEX_H 2048
static uint32_t *tex;

static void tex_make(void)
{
    tex = malloc((size_t)TEX_W * TEX_H * 4u);
    uint64_t s = 0x9e3779b97f4a7c15ull;
    for (int y = 0; y < TEX_H; y++)
        for (int x = 0; x < TEX_W; x++)
            tex[(size_t)y * TEX_W + (size_t)x] = 0xff000000u | (uint32_t)(40 + (x >> 5) % 64) << 16 | (uint32_t)(60 + (y >> 4) % 64) << 8 | (uint32_t)(90 + ((x + y) >> 6) % 64);
    /* Panels, then rows of small dark and light marks: what a desktop is made of. */
    for (int n = 0; n < 2600; n++) {
        const int w = 8 + (int)(vt_rand(&s) % 240), h = 6 + (int)(vt_rand(&s) % 90), x0 = (int)(vt_rand(&s) % (TEX_W - (uint64_t)w)), y0 = (int)(vt_rand(&s) % (TEX_H - (uint64_t)h));
        const uint32_t c = 0xff000000u | (uint32_t)(vt_rand(&s) & 0xffffff);
        for (int y = y0; y < y0 + h; y++)
            for (int x = x0; x < x0 + w; x++)
                tex[(size_t)y * TEX_W + (size_t)x] = c;
    }
    for (int n = 0; n < 90000; n++) {
        const int w = 1 + (int)(vt_rand(&s) % 7), h = 2 + (int)(vt_rand(&s) % 9), x0 = (int)(vt_rand(&s) % (TEX_W - 8)), y0 = (int)(vt_rand(&s) % (TEX_H - 12));
        const uint32_t c = (vt_rand(&s) & 1) ? 0xff101010u : 0xffe8e8e8u;
        for (int y = y0; y < y0 + h; y++)
            for (int x = x0; x < x0 + w; x++)
                tex[(size_t)y * TEX_W + (size_t)x] = c;
    }
}

static inline uint32_t tex_at(int x, int y) { return tex[(size_t)((y % TEX_H + TEX_H) % TEX_H) * TEX_W + (size_t)((x % TEX_W + TEX_W) % TEX_W)]; }

enum { S_STILL, S_WINDOW, S_PAN, S_SCROLL, S_FAST, S_PAN7, S_SCROLL7, S_BOTH53 };

/* Frame k of the base rate: every speed below is a whole number of pixels a base frame, so a given pair's half way is exact. */
/* The odd scenes: the texture read as a picture twice the size, box-filtered down, at positions in half pixels: an odd number of pixels a given frame, and the frame half way sits on a half pixel, as real motion does. */
static void scene_half(int hx, int hy, const struct bgra *d)
{
    for (int y = 0; y < H; y++) {
        uint32_t *r = bgra_row(d, y);
        for (int x = 0; x < W; x++) {
            const uint32_t t0 = tex_at(2 * x + hx, 2 * y + hy), t1 = tex_at(2 * x + 1 + hx, 2 * y + hy), t2 = tex_at(2 * x + hx, 2 * y + 1 + hy), t3 = tex_at(2 * x + 1 + hx, 2 * y + 1 + hy);
            uint32_t v = 0xff000000u;
            for (int s = 0; s < 24; s += 8)
                v |= ((((t0 >> s) & 255u) + ((t1 >> s) & 255u) + ((t2 >> s) & 255u) + ((t3 >> s) & 255u) + 2u) >> 2) << s;
            r[x] = v;
        }
    }
}

static void scene_render(int scene, int k, const struct bgra *d)
{
    if (scene == S_PAN7 || scene == S_SCROLL7 || scene == S_BOTH53) {
        scene_half(scene == S_SCROLL7 ? 0 : scene == S_PAN7 ? 7 * k : 5 * k, scene == S_PAN7 ? 0 : scene == S_SCROLL7 ? 7 * k : 3 * k, d);
        return;
    }
    for (int y = 0; y < H; y++) {
        uint32_t *r = bgra_row(d, y);
        for (int x = 0; x < W; x++) {
            int sx = x, sy = y;
            switch (scene) {
            case S_WINDOW: {
                const int wx = 80 + 6 * k, wy = 90 + 2 * k;
                if (x >= wx && x < wx + 520 && y >= wy && y < wy + 320) {
                    sx = 2000 + x - wx;
                    sy = 900 + y - wy;
                }
                break;
            }
            case S_PAN:
                sx = x + 6 * k;
                break;
            case S_SCROLL:
                if (x >= 300 && x < 1100)
                    sy = y + 10 * k;
                break;
            case S_FAST: {
                const int wx = 40 + 45 * k, wy = 300;
                if (x >= wx && x < wx + 120 && y >= wy && y < wy + 120) {
                    sx = 3000 + x - wx;
                    sy = 1200 + y - wy;
                }
                break;
            }
            default:
                break;
            }
            r[x] = tex_at(sx, sy);
        }
    }
}

/* ---- frames from a file ---- */

static const uint8_t *file_px;
static size_t file_frames;

static void file_frame(int k, const struct bgra *d)
{
    const uint32_t *s = (const uint32_t *)(file_px + (size_t)k * (size_t)W * (size_t)H * 4u);
    for (int y = 0; y < H; y++) {
        uint32_t *r = bgra_row(d, y);
        for (int x = 0; x < W; x++)
            r[x] = s[(size_t)y * (size_t)W + (size_t)x] | 0xff000000u;
    }
}

/* ---- comparison ---- */

struct sse {
    double gen, avg, rep;
    uint64_t n;
    double grad_gen, grad_truth, grad_avg;
    double seam_gen, inner_gen, seam_truth, inner_truth;
};

static void sse_add(struct sse *q, const struct bgra *gen, const struct bgra *truth, const struct bgra *a, const struct bgra *b, int x0, int y0, int x1, int y1)
{
    /* Seams: the step in green across the 8 pixel grid against the step inside the blocks, in the generated picture and in the truth. */
    for (int y = y0; y < y1; y++) {
        const uint32_t *g = bgra_row(gen, y), *t = bgra_row(truth, y);
        for (int x = x0 > 0 ? x0 : 1; x < x1; x++) {
            const double dg = abs((int)((g[x] >> 8) & 255u) - (int)((g[x - 1] >> 8) & 255u)), dt = abs((int)((t[x] >> 8) & 255u) - (int)((t[x - 1] >> 8) & 255u));
            if (x % 8 == 0) {
                q->seam_gen += dg;
                q->seam_truth += dt;
            } else {
                q->inner_gen += dg / 7.0;
                q->inner_truth += dt / 7.0;
            }
        }
    }
    /* Sharpness: the green channel's differences to the right and below, generated and the mean of the two against the truth. 1.000 is as sharp as what really happened. */
    for (int y = y0; y < y1 - 1; y++) {
        const uint32_t *g = bgra_row(gen, y) + x0, *t = bgra_row(truth, y) + x0, *pa = bgra_row(a, y) + x0, *pb = bgra_row(b, y) + x0;
        const uint32_t *g2 = bgra_row(gen, y + 1) + x0, *t2 = bgra_row(truth, y + 1) + x0, *pa2 = bgra_row(a, y + 1) + x0, *pb2 = bgra_row(b, y + 1) + x0;
#define G_OF(w) ((int)(((w) >> 8) & 255u))
#define MEAN_OF(u, v) ((G_OF(u) + G_OF(v) + 1) >> 1)
        for (int i = 0; i < x1 - x0 - 1; i++) {
            q->grad_gen += abs(G_OF(g[i + 1]) - G_OF(g[i])) + abs(G_OF(g2[i]) - G_OF(g[i]));
            q->grad_truth += abs(G_OF(t[i + 1]) - G_OF(t[i])) + abs(G_OF(t2[i]) - G_OF(t[i]));
            q->grad_avg += abs(MEAN_OF(pa[i + 1], pb[i + 1]) - MEAN_OF(pa[i], pb[i])) + abs(MEAN_OF(pa2[i], pb2[i]) - MEAN_OF(pa[i], pb[i]));
        }
#undef MEAN_OF
#undef G_OF
    }
    for (int y = y0; y < y1; y++) {
        const uint8_t *g = (const uint8_t *)(bgra_row(gen, y) + x0), *t = (const uint8_t *)(bgra_row(truth, y) + x0), *pa = (const uint8_t *)(bgra_row(a, y) + x0), *pb = (const uint8_t *)(bgra_row(b, y) + x0);
        for (int i = 0; i < (x1 - x0) * 4; i++) {
            if ((i & 3) == 3)
                continue;
            const int dg = g[i] - t[i], dr = pa[i] - t[i], da = ((pa[i] + pb[i] + 1) >> 1) - t[i];
            q->gen += (double)(dg * dg);
            q->rep += (double)(dr * dr);
            q->avg += (double)(da * da);
            q->n++;
        }
    }
}

static double db(double sse, uint64_t n) { return n && sse > 0.0 ? 10.0 * log10(255.0 * 255.0 * (double)n / sse) : 99.0; }

static int cmp_u32(const void *a, const void *b)
{
    const uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return (x > y) - (x < y);
}
static uint32_t median_of(uint32_t *v, int n)
{
    if (!n)
        return 0;
    qsort(v, (size_t)n, sizeof *v, cmp_u32);
    return v[n / 2];
}

/* The lane model on a few of the host's threads: new ones each round, a frame has some thirty. The answer does not depend on how many. */
struct pool_job {
    flow_fn fn;
    void *arg;
    int tid, n;
};
static void *pool_main(void *p)
{
    const struct pool_job *j = p;
    j->fn(j->arg, j->tid, j->n);
    return NULL;
}
static void pool_runner(void *ctx, flow_fn fn, void *arg)
{
    const int n = *(const int *)ctx;
    pthread_t t[32];
    struct pool_job jobs[32];
    for (int i = 1; i < n; i++) {
        jobs[i] = (struct pool_job){fn, arg, i, n};
        pthread_create(&t[i], NULL, pool_main, &jobs[i]);
    }
    fn(arg, 0, n);
    for (int i = 1; i < n; i++)
        pthread_join(t[i], NULL);
}

int main(int argc, char **argv)
{
    const char *scene_name = "window", *path = NULL, *form_name = "fused";
    int gap = 2, count = 12, tpc = 2, cores = 60, ref = 0;
    int model_threads = 0, pick_r = -1, pick_h = -1, pick_bias = -1, enough = -1, fine = 0, phase = -1;
    int react = -1;
    int stages = 0, tc = -1, beta = -1;
    const char *dump = NULL;
    int dump_x = 400, dump_y = 200;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--scene") && i + 1 < argc)
            scene_name = argv[++i];
        else if (!strcmp(argv[i], "--file") && i + 1 < argc)
            path = argv[++i];
        else if (!strcmp(argv[i], "--gap") && i + 1 < argc)
            gap = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--count") && i + 1 < argc)
            count = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--tpc") && i + 1 < argc)
            tpc = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--cores") && i + 1 < argc)
            cores = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--form") && i + 1 < argc)
            form_name = argv[++i];
        else if (!strcmp(argv[i], "--dump") && i + 3 < argc) {
            dump = argv[++i];
            dump_x = atoi(argv[++i]);
            dump_y = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--model") && i + 1 < argc)
            model_threads = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--pick-r") && i + 1 < argc)
            pick_r = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--pick-h") && i + 1 < argc)
            pick_h = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--pick-bias") && i + 1 < argc)
            pick_bias = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--phase") && i + 1 < argc)
            phase = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--fine") && i + 1 < argc)
            fine = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--react") && i + 1 < argc)
            react = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--stages") && i + 1 < argc)
            stages = atoi(argv[++i]) + 1;
        else if (!strcmp(argv[i], "--tc") && i + 1 < argc)
            tc = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--beta") && i + 1 < argc)
            beta = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--enough") && i + 1 < argc)
            enough = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--ref"))
            ref = 1;
        else {
            fprintf(stderr, "usage: mf_replay [--scene still|window|pan|scroll|fast|pan7|scroll7|both53 | --file F.bgra] [--gap G] [--count N] [--tpc T] [--cores C] [--form fused|plain|pixel] [--ref] [--model THREADS] [--pick-r R] [--pick-h H] [--pick-bias Q6] [--enough Q6] [--fine S] [--phase P] [--stages MASK] [--tc T] [--beta B]\n");
            return 2;
        }
    }
    const int form = !strcmp(form_name, "plain") ? FLOW_PLAIN : !strcmp(form_name, "pixel") ? FLOW_PIXEL : FLOW_FUSED;
    int scene = S_WINDOW;
    if (!strcmp(scene_name, "still"))
        scene = S_STILL;
    else if (!strcmp(scene_name, "pan"))
        scene = S_PAN;
    else if (!strcmp(scene_name, "scroll"))
        scene = S_SCROLL;
    else if (!strcmp(scene_name, "fast"))
        scene = S_FAST;
    else if (!strcmp(scene_name, "pan7"))
        scene = S_PAN7;
    else if (!strcmp(scene_name, "scroll7"))
        scene = S_SCROLL7;
    else if (!strcmp(scene_name, "both53"))
        scene = S_BOTH53;
    if (path) {
        const int fd = open(path, O_RDONLY);
        struct stat st;
        if (fd < 0 || fstat(fd, &st)) {
            perror(path);
            return 2;
        }
        file_px = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_SHARED, fd, 0);
        file_frames = (size_t)st.st_size / ((size_t)W * (size_t)H * 4u);
        scene_name = path;
        if ((size_t)((count - 1) * gap + 1) > file_frames)
            count = (int)((file_frames - 1) / (size_t)gap) + 1;
    } else {
        tex_make();
    }
    struct mf_link l;
    if (model_threads) {
        /* No card, no session: the part is plain memory here. */
        if (posix_memalign((void **)&l.part, 4096, (size_t)MF_PART_BYTES))
            return 1;
        l.ctl = (struct mf_ctl *)l.part;
        memset(l.part, 0, (size_t)MF_IN_OFFSET);
        ref = 1;
    } else if (mf_link_open(&l))
        return 1;
    const struct mf_config cfg = {.w = (uint32_t)W, .h = (uint32_t)H, .cores = (uint32_t)cores, .tpc = (uint32_t)tpc, .form = (uint32_t)form, .pick_r = (uint32_t)(pick_r + 1), .pick_h = (uint32_t)(pick_h + 1), .pick_bias = (uint32_t)(pick_bias + 1), .enough = (uint32_t)(enough + 1), .fine = (uint32_t)fine, .phase = (uint32_t)(phase + 1), .stages = (uint32_t)stages, .tc = (uint32_t)(tc + 1), .beta = (uint32_t)(beta + 1), .alpha = (uint32_t)(react + 1)};
    if (!model_threads && mf_session_start(&l, &cfg, 30.0))
        return 1;
    if (!model_threads)
        printf("session: %u cores, %u threads, tsc %u MHz, form %s\n", l.ctl->card_cores, l.ctl->card_threads, l.ctl->card_mhz, form_name);
    struct flow *model = NULL;
    if (ref) {
        if (posix_memalign((void **)&model, 64, sizeof *model))
            return 1;
        void *mem;
        if (posix_memalign(&mem, 4096, flow_bytes(W, H, form)))
            return 1;
        memset(mem, 0, flow_bytes(W, H, form));
        static int pool_n;
        pool_n = model_threads ? model_threads : 8;
        if (flow_init(model, mem, W, H, form, pool_runner, &pool_n))
            return 1;
        if (phase >= 0)
            model->pick_phase = phase;
        if (fine)
            model->fine_stride = fine;
        if (pick_r >= 0)
            model->pick_r = pick_r;
        if (pick_h >= 0)
            model->pick_h = pick_h;
        if (pick_bias >= 0)
            model->pick_bias = (float)pick_bias;
        if (enough >= 0)
            model->fg.enough = (float)enough;
        if (react >= 0)
            fsr_set_alpha_floor(model->fsr, (long double)react / 100.0L);
        if (stages) {
            model->use_fsr = ((stages - 1) & 1) != 0;
            model->use_deblock = ((stages - 1) & 2) != 0;
        }
        if (tc >= 0)
            model->db_tc = (float)tc;
        if (beta >= 0)
            model->db_beta = (float)beta;
    }
    void *tm, *hm, *am;
    if (posix_memalign(&tm, 64, bgra_bytes(W, H)) || posix_memalign(&hm, 64, bgra_bytes(W, H)) || posix_memalign(&am, 64, bgra_bytes(W, H)))
        return 1;
    const struct bgra truth = bgra_at(tm, W, H), newer = bgra_at(hm, W, H), older = bgra_at(am, W, H);
    struct sse whole, moving;
    memset(&whole, 0, sizeof whole);
    memset(&moving, 0, sizeof moving);
    uint64_t diff_px = 0, compared_px = 0, moved_mismatch = 0, control_caught = 0;
    uint64_t moved_sum = 0;
    int made_n = 0, scene_n = 0;
    static uint32_t us[MF_T_COUNT][4096];
    struct mf_ctl *c = l.ctl;
    for (int i = 0; i < count; i++) {
        const int k = i * gap;
        const uint32_t slot = (uint32_t)i % MF_SLOTS;
        const struct bgra in = bgra_at(l.part + mf_in_offset(slot), W, H), out = bgra_at(l.part + mf_out_offset(slot), W, H);
        if (path)
            file_frame(k, &in);
        else
            scene_render(scene, k, &in);
        memcpy(bgra_base(&newer), bgra_base(&in), bgra_bytes(W, H));
        const uint64_t seq = (uint64_t)i + 1;
        struct mf_gen g;
        memset(&g, 0, sizeof g);
        if (!model_threads) {
            struct mf_msg *m = &c->msg[seq % MF_RING];
            m->seq = seq;
            m->in_slot = slot;
            m->out_slot = slot;
            m->flags = i == 0 ? MF_NO_MID : 0;
            __atomic_thread_fence(__ATOMIC_SEQ_CST);
            c->frames_pub = seq;
            const uint64_t t0 = mf_now_ns();
            while (c->gen_pub < seq) {
                if ((double)(mf_now_ns() - t0) * 1e-9 > 20.0) {
                    fprintf(stderr, "no answer from the card for frame %d\n", i);
                    return 1;
                }
                struct timespec nap = {0, 100000};
                nanosleep(&nap, NULL);
            }
            g = c->gen[seq % MF_RING];
        }
        for (int t = 0; t < MF_T_COUNT; t++)
            if (g.made == 1 && made_n < 4096)
                us[t][made_n] = g.us[t];
        if (model) {
            memcpy(bgra_base(flow_next_frame(model)), bgra_base(&in), bgra_bytes(W, H));
            flow_land(model);
            const int mm = i ? flow_make(model) : -1;
            if (form == FLOW_PIXEL) {
                if (mm == 1)
                    flow_finish_generated(model);
                else
                    flow_no_generated(model);
            }
            const struct bgra *mout = form == FLOW_PIXEL && mm == 1 ? model->final : &model->out;
            if (model_threads) {
                g.made = mm;
                g.moved = model->moved;
                if (mm == 1)
                    memcpy(bgra_base(&out), bgra_base(mout), bgra_bytes(W, H));
            }
            if (mm != g.made)
                printf("frame %d: card made %d, model %d\n", i, g.made, mm);
            if (mm == 1) {
                if (model->moved != g.moved)
                    moved_mismatch++;
                for (int y = 0; y < H; y++) {
                    const uint32_t *pa = bgra_row(&out, y), *pb = bgra_row(mout, y);
                    for (int x = 0; x < W; x++)
                        diff_px += pa[x] != pb[x];
                }
                compared_px += (uint64_t)W * (uint64_t)H;
                /* The control: the same comparison on a copy with one byte flipped must see it. */
                const uint32_t keep = bgra_row(mout, H / 2)[W / 2];
                bgra_row(mout, H / 2)[W / 2] = keep ^ 1u;
                control_caught += bgra_row(&out, H / 2)[W / 2] != bgra_row(mout, H / 2)[W / 2];
                bgra_row(mout, H / 2)[W / 2] = keep;
            }
        }
        if (g.made == 0)
            scene_n++;
        if (g.made == 1) {
            moved_sum += g.moved;
            made_n++;
            if (dump && made_n == 3 && gap % 2 == 0) {
                /* A close look: the generated picture, the truth and their difference side by side, at a crop of the picture. */
                if (path)
                    file_frame(k - gap / 2, &truth);
                else
                    scene_render(scene, k - gap / 2, &truth);
                char name[256];
                snprintf(name, sizeof name, "%s.ppm", dump);
                FILE *fp = fopen(name, "wb");
                if (dump_x < 0) {
                    /* The whole picture's difference alone, four times over, and where it sits. */
                    fprintf(fp, "P6\n%d %d\n255\n", W, H);
                    double edge = 0.0, inner = 0.0;
                    for (int y = 0; y < H; y++)
                        for (int x = 0; x < W; x++) {
                            const uint32_t pg = bgra_row(&out, y)[x], pt = bgra_row(&truth, y)[x];
                            uint8_t rgb[3];
                            for (int q = 0; q < 3; q++) {
                                const int e = (int)((pg >> (16 - 8 * q)) & 255u) - (int)((pt >> (16 - 8 * q)) & 255u), d = abs(e) * 4;
                                rgb[q] = (uint8_t)(d > 255 ? 255 : d);
                                if (x < 32 || x >= W - 32 || y < 32 || y >= H - 32)
                                    edge += (double)(e * e);
                                else
                                    inner += (double)(e * e);
                            }
                            fwrite(rgb, 1, 3, fp);
                        }
                    printf("error: %.1f %% within 32 px of the picture's edge\n", 100.0 * edge / (edge + inner + 1e-9));
                    fclose(fp);
                    fp = NULL;
                }
                const int cw = 400, ch = 300;
                if (!fp)
                    goto dumped;
                fprintf(fp, "P6\n%d %d\n255\n", 3 * cw, ch);
                for (int y = 0; y < ch; y++)
                    for (int v = 0; v < 3; v++)
                        for (int x = 0; x < cw; x++) {
                            const uint32_t pg = bgra_row(&out, y + dump_y)[x + dump_x], pt = bgra_row(&truth, y + dump_y)[x + dump_x];
                            const uint32_t pd = v == 0 ? pg : pt;
                            uint8_t rgb[3];
                            for (int q = 0; q < 3; q++) {
                                const int a1 = (int)((pg >> (16 - 8 * q)) & 255u), a2 = (int)((pt >> (16 - 8 * q)) & 255u), d = abs(a1 - a2) * 4;
                                rgb[q] = v == 2 ? (uint8_t)(d > 255 ? 255 : d) : (uint8_t)((pd >> (16 - 8 * q)) & 255u);
                            }
                            fwrite(rgb, 1, 3, fp);
                        }
                fclose(fp);
            dumped:;
            }
            if (gap >= 2 && gap % 2 == 0) {
                if (path)
                    file_frame(k - gap / 2, &truth);
                else
                    scene_render(scene, k - gap / 2, &truth);
                sse_add(&whole, &out, &truth, &older, &newer, 0, 0, W, H);
                for (int ty = 0; ty + 16 <= H; ty += 16)
                    for (int tx = 0; tx + 16 <= W; tx += 16) {
                        uint64_t d = 0;
                        for (int y = ty; y < ty + 16; y++) {
                            const uint8_t *pa = (const uint8_t *)(bgra_row(&older, y) + tx), *pb = (const uint8_t *)(bgra_row(&newer, y) + tx);
                            for (int q = 0; q < 64; q++)
                                d += (uint64_t)abs(pa[q] - pb[q]);
                        }
                        if (d >= 2u * 16u * 16u * 3u)
                            sse_add(&moving, &out, &truth, &older, &newer, tx, ty, tx + 16, ty + 16);
                    }
            }
        }
        memcpy(bgra_base(&older), bgra_base(&newer), bgra_bytes(W, H));
    }
    if (!model_threads)
        mf_session_end(&l, 5.0);
    printf("%s gap %d: %d frames, %d generated, %d scene changes, %.1f %% blocks moved\n", scene_name, gap, count, made_n, scene_n, made_n ? 100.0 * (double)moved_sum / (double)made_n / (double)(form == FLOW_PIXEL ? ((W + 7) / 8) * ((H + 7) / 8) : ((W + 15) / 16) * ((H + 15) / 16)) : 0.0);
    if (whole.n)
        printf("dB whole  generated %.3f, mean of two %.3f, older again %.3f\n", db(whole.gen, whole.n), db(whole.avg, whole.n), db(whole.rep, whole.n));
    if (moving.n)
        printf("dB moving generated %.3f, mean of two %.3f, older again %.3f (%.1f %% of samples)\n", db(moving.gen, moving.n), db(moving.avg, moving.n), db(moving.rep, moving.n), 100.0 * (double)moving.n / (double)whole.n);
    if (moving.n)
        printf("sharpness moving, against the truth: generated %.3f, mean of two %.3f\n", moving.grad_gen / moving.grad_truth, moving.grad_avg / moving.grad_truth);
    if (moving.n) {
        printf("seams moving, 1.000 = the truth's: generated %.3f\n", (moving.seam_gen / moving.inner_gen) / (moving.seam_truth / moving.inner_truth));
    }
    static const char *const names[MF_T_COUNT] = {"dma_in", "land", "pyr", "hist", "search", "filter", "up", "mid", "blend", "fsr", "deblock", "dma_out", "total"};
    if (!model_threads) {
    printf("card us, median:");
    for (int t = 0; t < MF_T_COUNT; t++) {
        uint32_t mx = 0;
        for (int n = 0; n < made_n; n++)
            mx = us[t][n] > mx ? us[t][n] : mx;
        printf(" %s %u", names[t], median_of(us[t], made_n));
        if (t == MF_T_TOTAL)
            printf(" (max %u)", mx);
    }
    printf("\n");
    }
    if (ref && !model_threads) {
        printf("card = lane model: %llu of %llu pixels differ, moved mismatches %llu; control caught %llu of %d\n", (unsigned long long)diff_px, (unsigned long long)compared_px, (unsigned long long)moved_mismatch, (unsigned long long)control_caught, made_n);
        if (diff_px || moved_mismatch || (int)control_caught != made_n)
            return 1;
    }
    return 0;
}

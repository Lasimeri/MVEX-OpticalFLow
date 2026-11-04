/* mf_replay: the gate. Frames go through the real path (the part, the card's DMA, mvexflowd, back), and each generated picture is held against:
 *   --ref    the same kernels on the host's lane model, byte for byte over every visible pixel (a control flips one byte to prove the comparison can fail);
 *   truth    a real frame left out between the two given: dB against it for the generated picture, for the mean of the two frames and for the older frame again, over the whole picture and over tiles that moved.
 *
 * Frames come from a synthetic scene (exact truth at every half step) or a raw BGRA file. No window, nothing on the monitor. */
#define _GNU_SOURCE
#include <fcntl.h>
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

enum { S_STILL, S_WINDOW, S_PAN, S_SCROLL, S_FAST };

/* Frame k of the base rate: every speed below is a whole number of pixels a base frame, so a given pair's half way is exact. */
static void scene_render(int scene, int k, const struct bgra *d)
{
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
};

static void sse_add(struct sse *q, const struct bgra *gen, const struct bgra *truth, const struct bgra *a, const struct bgra *b, int x0, int y0, int x1, int y1)
{
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

static void one_runner(void *ctx, flow_fn fn, void *arg)
{
    (void)ctx;
    fn(arg, 0, 1);
}

int main(int argc, char **argv)
{
    const char *scene_name = "window", *path = NULL, *form_name = "fused";
    int gap = 2, count = 12, tpc = 2, cores = 60, ref = 0;
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
        else if (!strcmp(argv[i], "--ref"))
            ref = 1;
        else {
            fprintf(stderr, "usage: mf_replay [--scene still|window|pan|scroll|fast | --file F.bgra] [--gap G] [--count N] [--tpc T] [--cores C] [--form fused|plain] [--ref]\n");
            return 2;
        }
    }
    const int form = !strcmp(form_name, "plain") ? FLOW_PLAIN : FLOW_FUSED;
    int scene = S_WINDOW;
    if (!strcmp(scene_name, "still"))
        scene = S_STILL;
    else if (!strcmp(scene_name, "pan"))
        scene = S_PAN;
    else if (!strcmp(scene_name, "scroll"))
        scene = S_SCROLL;
    else if (!strcmp(scene_name, "fast"))
        scene = S_FAST;
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
    if (mf_link_open(&l))
        return 1;
    const struct mf_config cfg = {.w = (uint32_t)W, .h = (uint32_t)H, .cores = (uint32_t)cores, .tpc = (uint32_t)tpc, .form = (uint32_t)form};
    if (mf_session_start(&l, &cfg, 30.0))
        return 1;
    printf("session: %u cores, %u threads, tsc %u MHz, form %s\n", l.ctl->card_cores, l.ctl->card_threads, l.ctl->card_mhz, form_name);
    struct flow *model = NULL;
    if (ref) {
        if (posix_memalign((void **)&model, 64, sizeof *model))
            return 1;
        void *mem;
        if (posix_memalign(&mem, 4096, flow_bytes(W, H, form)))
            return 1;
        memset(mem, 0, flow_bytes(W, H, form));
        if (flow_init(model, mem, W, H, form, one_runner, NULL))
            return 1;
    }
    void *tm, *hm, *am;
    if (posix_memalign(&tm, 64, bgra_bytes(W, H)) || posix_memalign(&hm, 64, bgra_bytes(W, H)) || posix_memalign(&am, 64, bgra_bytes(W, H)))
        return 1;
    const struct bgra truth = bgra_at(tm, W, H), newer = bgra_at(hm, W, H), older = bgra_at(am, W, H);
    struct sse whole = {0, 0, 0, 0}, moving = {0, 0, 0, 0};
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
        const struct mf_gen g = c->gen[seq % MF_RING];
        for (int t = 0; t < MF_T_COUNT; t++)
            if (g.made == 1 && made_n < 4096)
                us[t][made_n] = g.us[t];
        if (g.made == 1)
            moved_sum += g.moved;
        if (g.made == 0)
            scene_n++;
        if (model) {
            memcpy(bgra_base(flow_next_frame(model)), bgra_base(&in), bgra_bytes(W, H));
            flow_land(model);
            const int mm = i ? flow_make(model) : -1;
            if (mm != g.made)
                printf("frame %d: card made %d, model %d\n", i, g.made, mm);
            if (mm == 1) {
                if (model->moved != g.moved)
                    moved_mismatch++;
                for (int y = 0; y < H; y++) {
                    const uint32_t *pa = bgra_row(&out, y), *pb = bgra_row(&model->out, y);
                    for (int x = 0; x < W; x++)
                        diff_px += pa[x] != pb[x];
                }
                compared_px += (uint64_t)W * (uint64_t)H;
                /* The control: the same comparison on a copy with one byte flipped must see it. */
                const uint32_t keep = bgra_row(&model->out, H / 2)[W / 2];
                bgra_row(&model->out, H / 2)[W / 2] = keep ^ 1u;
                control_caught += bgra_row(&out, H / 2)[W / 2] != bgra_row(&model->out, H / 2)[W / 2];
                bgra_row(&model->out, H / 2)[W / 2] = keep;
            }
        }
        if (g.made == 1) {
            made_n++;
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
    mf_session_end(&l, 5.0);
    printf("%s gap %d: %d frames, %d generated, %d scene changes, %.1f %% blocks moved\n", scene_name, gap, count, made_n, scene_n, made_n ? 100.0 * (double)moved_sum / (double)made_n / (double)(((W + 15) / 16) * ((H + 15) / 16)) : 0.0);
    if (whole.n)
        printf("dB whole  generated %.3f, mean of two %.3f, older again %.3f\n", db(whole.gen, whole.n), db(whole.avg, whole.n), db(whole.rep, whole.n));
    if (moving.n)
        printf("dB moving generated %.3f, mean of two %.3f, older again %.3f (%.1f %% of samples)\n", db(moving.gen, moving.n), db(moving.avg, moving.n), db(moving.rep, moving.n), 100.0 * (double)moving.n / (double)whole.n);
    static const char *const names[MF_T_COUNT] = {"dma_in", "land", "pyr", "hist", "search", "filter", "up", "mid", "blend", "dma_out", "total"};
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
    if (ref) {
        printf("card = lane model: %llu of %llu pixels differ, moved mismatches %llu; control caught %llu of %d\n", (unsigned long long)diff_px, (unsigned long long)compared_px, (unsigned long long)moved_mismatch, (unsigned long long)control_caught, made_n);
        if (diff_px || moved_mismatch || (int)control_caught != made_n)
            return 1;
    }
    return 0;
}

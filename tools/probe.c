/* probe.c: answers, by counting pixels and keeping none, what the whole design rests on.
 *
 *   A  Shown 1:1?          The overlay shows a one-pixel checker with a coded ramp; the output's own screencast is compared with it, pixel for pixel. Any resampling of the surface at scale 0.75 shows up as misses.
 *   B  Hidden from the cast? KWin's script sets excludeFromCapture on this process's surface (scripts/excl.sh); the same count must then fall to zero.
 *   C  What rate does the cast give, with the overlay idle, and with the overlay committing every refresh?
 *
 * Run: desk cast --output NAME --no-cursor -- out/host/probe @NODE@ NAME */
#define _GNU_SOURCE
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "pwc.h"
#include "wlp.h"

#define W 1366
#define H 768
#define STRIDE (W * 4)

static uint32_t *pattern;
static _Atomic uint64_t frames, same_px, total_px, last_t, first_t, alpha_or, alpha_and;

static uint64_t now_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

static void on_frame(void *u, const uint8_t *data, int stride, int w, int h, uint64_t pts, uint64_t t)
{
    (void)u, (void)pts;
    uint64_t same = 0;
    uint32_t aor = 0, aand = 0xffffffffu;
    for (int y = 0; y < h && y < H; y++) {
        const uint32_t *r = (const uint32_t *)(data + (size_t)y * (size_t)stride), *p = pattern + (size_t)y * W;
        for (int x = 0; x < w && x < W; x++) {
            same += (r[x] & 0xffffffu) == (p[x] & 0xffffffu);
            aor |= r[x];
            aand &= r[x];
        }
    }
    atomic_fetch_add(&same_px, same);
    atomic_fetch_add(&total_px, (uint64_t)w * (uint64_t)h);
    atomic_fetch_or(&alpha_or, aor);
    atomic_fetch_and(&alpha_and, aand);
    if (!atomic_load(&first_t))
        atomic_store(&first_t, t);
    atomic_store(&last_t, t);
    atomic_fetch_add(&frames, 1);
}

static int presented, discarded;
static uint64_t last_pres, gap_min = ~0ull, gap_max, gap_n, gap_sum;
static void on_presented(void *u, uint64_t c, uint64_t t, uint32_t rf, uint64_t v, uint32_t fl)
{
    (void)u, (void)c, (void)rf, (void)v, (void)fl;
    presented++;
    if (last_pres) {
        const uint64_t g = t - last_pres;
        gap_min = g < gap_min ? g : gap_min;
        gap_max = g > gap_max ? g : gap_max;
        gap_sum += g;
        gap_n++;
    }
    last_pres = t;
}
static void on_discarded(void *u, uint64_t c)
{
    (void)u, (void)c;
    discarded++;
}

static void reset_counts(void)
{
    atomic_store(&frames, 0);
    atomic_store(&same_px, 0);
    atomic_store(&total_px, 0);
    atomic_store(&first_t, 0);
    atomic_store(&last_t, 0);
    atomic_store(&alpha_or, 0);
    atomic_store(&alpha_and, 0xffffffffu);
}

static void report(const char *what)
{
    const uint64_t n = atomic_load(&frames), s = atomic_load(&same_px), t = atomic_load(&total_px), a = atomic_load(&first_t), b = atomic_load(&last_t);
    printf("%-34s %4llu frames, %7.3f /s; pattern pixels %llu of %llu (%.4f %%); x byte or %02x and %02x\n", what, (unsigned long long)n, n > 1 && b > a ? (double)(n - 1) * 1e9 / (double)(b - a) : 0.0, (unsigned long long)s, (unsigned long long)t,
           t ? 100.0 * (double)s / (double)t : 0.0, (unsigned)(atomic_load(&alpha_or) >> 24), (unsigned)(atomic_load(&alpha_and) >> 24));
    fflush(stdout);
}

static void pump(struct wlp *p, double seconds, int commit_a, int commit_b)
{
    const uint64_t end = now_ns() + (uint64_t)(seconds * 1e9);
    int flip = 0, pending = 0;
    int last_presented = presented;
    while (now_ns() < end) {
        struct pollfd q = {wlp_fd(p), POLLIN, 0};
        if (commit_a >= 0 && (pending == 0 || presented != last_presented)) {
            last_presented = presented;
            wlp_commit(p, flip ? commit_b : commit_a, 0, 0, H);
            flip = !flip;
            pending = 1;
        }
        wlp_flush(p);
        poll(&q, 1, 2);
        wlp_read(p);
    }
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: probe NODE OUTPUT (through desk cast)\n");
        return 2;
    }
    const uint32_t node = (uint32_t)atoi(argv[1]);
    /* Two buffers: the checker with a ramp, and its inverse for the rate test. */
    const size_t bytes = 2u * (size_t)STRIDE * H;
    const int fd = memfd_create("mvexflow-probe", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, (off_t)bytes))
        return 3;
    pattern = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (pattern == MAP_FAILED)
        return 3;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            const uint32_t chk = ((x ^ y) & 1) ? 0xf0u : 0x10u;
            const uint32_t v = 0xff000000u | chk << 8 | (uint32_t)(x & 255) << 16 | (uint32_t)(y & 255);
            pattern[(size_t)y * W + (size_t)x] = v;
            pattern[(size_t)(y + H) * W + (size_t)x] = v ^ 0x00ffffffu;
        }
    const struct wlp_cb cb = {NULL, on_presented, on_discarded, NULL};
    struct wlp *p = wlp_open(argv[2], W, H, 1, &cb);
    if (!p)
        return 4;
    int lw, lh;
    wlp_size(p, &lw, &lh);
    printf("surface configured %d x %d logical; output %d mHz\n", lw, lh, wlp_refresh_mhz(p));
    wlp_pool(p, fd, bytes);
    const int b0 = wlp_buffer(p, 0, STRIDE, 1), b1 = wlp_buffer(p, (uint64_t)STRIDE * H, STRIDE, 1);
    wlp_commit(p, b0, 0, 0, H);
    wlp_flush(p);
    pump(p, 0.5, -1, -1);
    const uint32_t mhz = (uint32_t)wlp_refresh_mhz(p);
    struct pwc *c = pwc_open(node, W, H, mhz, 2000, on_frame, NULL);
    if (!c)
        return 5;
    pump(p, 1.0, -1, -1);
    printf("cast format: %s\n", pwc_format(c));
    reset_counts();
    pump(p, 3.0, -1, -1);
    report("A shown, overlay idle");
    fflush(stdout);
    char cmd[256];
    snprintf(cmd, sizeof cmd, "scripts/excl.sh on %d", (int)getpid());
    if (system(cmd))
        printf("excl.sh failed\n");
    pump(p, 1.0, -1, -1);
    reset_counts();
    pump(p, 3.0, -1, -1);
    report("B excluded, overlay idle");
    reset_counts();
    presented = 0;
    gap_n = gap_sum = gap_max = 0;
    gap_min = ~0ull;
    pump(p, 5.0, b0, b1);
    report("C excluded, overlay every refresh");
    printf("presented %d, discarded %d; gaps ms min %.3f mean %.3f max %.3f over %llu\n", presented, discarded, (double)gap_min * 1e-6, gap_n ? (double)gap_sum / (double)gap_n * 1e-6 : 0.0, (double)gap_max * 1e-6, (unsigned long long)gap_n);
    if (system("scripts/excl.sh off"))
        printf("excl.sh off failed\n");
    pwc_close(c);
    wlp_close(p);
    return 0;
}

/* mvexflow: one monitor at half its rate, doubled by the card.
 *
 *   KWin's cast of the monitor, capped at half the refresh, no pointer  ─▶  this program's copy into the shared part (PipeWire's thread)  ─▶  the card makes the frame half way between the last two  ─▶  overlay: generated, real, generated, real.
 *
 * The overlay shows the two kinds of picture straight out of the shared part: a real frame where the copy put it, a generated one where the card's DMA put it. Nothing is copied after the one copy in. KWin hides the overlay from its own cast
 * (scripts/excl.sh), so the cast shows what lies under it; clicks go through; the pointer stays the compositor's own, on top, at the full refresh.
 *
 * Run through desk cast, which owns the permission to open the stream: desk cast --output NAME --no-cursor -- out/host/mvexflow --node @NODE@ --output NAME */
#define _GNU_SOURCE
#include <immintrin.h>
#include <poll.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include "mf_link.h"
#include "plane.h"
#include "pwc.h"
#include "wlp.h"

#define W_MAX_Q 16

enum { IN_WRITING = 1, IN_CARD = 2, IN_SHOWN = 4, IN_PREV = 8 };
enum { OUT_CARD = 1, OUT_SHOWN = 2 };

struct newframe {
    int slot, gap_ok;
    uint64_t pts, t_in;
    int y0, y1;
};

struct job {
    uint64_t seq;
    int in, out;
    uint64_t pts, t_in, t_pub;
    int late;
};

struct shown {
    uint64_t t_src, t_commit;
    int generated;
};

struct stats {
    uint64_t cast, dup, noslot;                                   /* PipeWire's thread: atomics below */
    uint64_t sent, made, scene, nomid, late;
    uint64_t com_g, com_r, skip_g;
    uint64_t pres_g, pres_r, disc;
    uint64_t cad[5];                                              /* pictures that followed the last by 0, 1, 2, 3, 4+ refreshes */
    uint64_t lat_sum, lat_n, lat_max, lat_min;
    uint64_t gap_max;
    uint64_t card_sum, card_n, card_max, moved_sum;
};

static volatile sig_atomic_t stop;
static void on_signal(int s)
{
    (void)s;
    stop = 1;
}

struct app {
    struct mf_link l;
    struct wlp *w;
    int w_px, h_px, stride_b;
    int buf_in[MF_SLOTS], buf_out[MF_SLOTS];
    int buffer_kind[WLP_MAX_BUFFERS], buffer_slot[WLP_MAX_BUFFERS];
    _Atomic uint32_t st_in[MF_SLOTS];
    uint32_t st_out[MF_SLOTS];
    int prev_in;
    uint64_t last_src_t, interval_ns;
    /* From PipeWire's thread to the main one. */
    struct newframe q[W_MAX_Q];
    _Atomic uint32_t q_head, q_tail;
    int ev;
    _Atomic uint64_t a_cast, a_dup, a_noslot;
    int pass_only;
    /* Main thread. */
    struct job jobs[MF_RING];
    uint64_t pub, done;
    int must_skip_mid;
    struct shown ring[256];
    uint64_t commits, shown_ok;

    uint64_t last_t;
    struct stats s;
    int out_turn;
};

/* The slot's pixel (0, 0) offset within the slot. */
static inline uint64_t px0(const struct app *a) { return (uint64_t)a->stride_b * VT_PAD_Y + VT_PAD_X * 4u; }

/* A row in, with the x byte forced to 255 so the overlay's alpha cannot go wrong, compared with the same row of the frame before. 0 when it is the same. */
__attribute__((target("avx2"))) static int copy_row(uint32_t *d, const uint32_t *s, const uint32_t *prev, int w)
{
    const __m256i alpha = _mm256_set1_epi32((int)0xff000000u);
    __m256i acc = _mm256_setzero_si256();
    int x = 0;
    if (prev) {
        for (; x + 8 <= w; x += 8) {
            const __m256i v = _mm256_or_si256(_mm256_loadu_si256((const __m256i *)(s + x)), alpha);
            _mm256_storeu_si256((__m256i *)(d + x), v);
            acc = _mm256_or_si256(acc, _mm256_xor_si256(v, _mm256_loadu_si256((const __m256i *)(prev + x))));
        }
    } else {
        for (; x + 8 <= w; x += 8)
            _mm256_storeu_si256((__m256i *)(d + x), _mm256_or_si256(_mm256_loadu_si256((const __m256i *)(s + x)), alpha));
    }
    uint32_t tail = 0;
    for (; x < w; x++) {
        d[x] = s[x] | 0xff000000u;
        tail |= prev ? d[x] ^ prev[x] : 1u;
    }
    return !prev || tail || !_mm256_testz_si256(acc, acc);
}

static void on_frame(void *u, const uint8_t *data, int stride, int w, int h, uint64_t pts, uint64_t t)
{
    struct app *a = u;
    atomic_fetch_add_explicit(&a->a_cast, 1, memory_order_relaxed);
    if (w != a->w_px || h != a->h_px)
        return;
    int s = -1;
    for (int i = 0; i < MF_SLOTS && s < 0; i++) {
        uint32_t want = 0;
        if (atomic_compare_exchange_strong(&a->st_in[i], &want, IN_WRITING))
            s = i;
    }
    if (s < 0) {
        atomic_fetch_add_explicit(&a->a_noslot, 1, memory_order_relaxed);
        return;
    }
    uint8_t *base = a->l.part + mf_in_offset((uint32_t)s) + px0(a);
    const uint8_t *prev = a->prev_in >= 0 ? a->l.part + mf_in_offset((uint32_t)a->prev_in) + px0(a) : NULL;
    int y0 = h, y1 = 0;
    for (int y = 0; y < h; y++)
        if (copy_row((uint32_t *)(base + (size_t)y * (size_t)a->stride_b), (const uint32_t *)(data + (size_t)y * (size_t)stride), prev ? (const uint32_t *)(prev + (size_t)y * (size_t)a->stride_b) : NULL, w)) {
            y0 = y < y0 ? y : y0;
            y1 = y + 1;
        }
    if (prev && y1 == 0) {
        /* Nothing changed on the monitor: nothing to make, nothing to show, nothing for the compositor to do. */
        atomic_store(&a->st_in[s], 0);
        atomic_fetch_add_explicit(&a->a_dup, 1, memory_order_relaxed);
        return;
    }
    const uint64_t src_t = pts ? pts : t;
    /* A mid is worth making across a captured frame that did not change (24 fps content at this cast: one gap in five), not across a pause. */
    struct newframe f = {s, a->last_src_t && src_t - a->last_src_t <= 2 * a->interval_ns + a->interval_ns / 2, src_t, t, y0, y1};
    a->last_src_t = src_t;
    if (a->prev_in >= 0)
        atomic_fetch_and(&a->st_in[a->prev_in], ~(uint32_t)IN_PREV);
    a->prev_in = s;
    atomic_store(&a->st_in[s], IN_PREV | IN_WRITING);
    const uint32_t tail = atomic_load_explicit(&a->q_tail, memory_order_relaxed);
    if (tail - atomic_load_explicit(&a->q_head, memory_order_acquire) >= W_MAX_Q) {
        atomic_store(&a->st_in[s], IN_PREV);
        atomic_fetch_add_explicit(&a->a_noslot, 1, memory_order_relaxed);
        return;
    }
    a->q[tail % W_MAX_Q] = f;
    atomic_store_explicit(&a->q_tail, tail + 1, memory_order_release);
    const uint64_t one = 1;
    if (write(a->ev, &one, 8) != 8)
        return;
}

static void on_presented(void *u, uint64_t c, uint64_t t, uint32_t refresh, uint64_t vblank, uint32_t flags)
{
    (void)flags, (void)vblank;
    struct app *a = u;
    const struct shown *e = &a->ring[c & 255];
    a->shown_ok++;
    if (e->generated)
        a->s.pres_g++;
    else
        a->s.pres_r++;
    /* The cadence, in refreshes between pictures, from the instants themselves: the compositor's vblank counter does not count here. */
    if (a->last_t) {
        const uint64_t gap = t - a->last_t, slots = (gap + refresh / 2) / (refresh ? refresh : 1);
        a->s.cad[slots > 4 ? 4 : slots]++;
        a->s.gap_max = gap > a->s.gap_max ? gap : a->s.gap_max;
    }
    a->last_t = t;
    if (e->t_src && t > e->t_src) {
        const uint64_t lat = t - e->t_src;
        a->s.lat_sum += lat;
        a->s.lat_n++;
        a->s.lat_max = lat > a->s.lat_max ? lat : a->s.lat_max;
        a->s.lat_min = !a->s.lat_min || lat < a->s.lat_min ? lat : a->s.lat_min;
    }
}

static void on_discarded(void *u, uint64_t c)
{
    (void)c;
    struct app *a = u;
    a->shown_ok++;
    a->s.disc++;
}

static void on_released(void *u, int b)
{
    struct app *a = u;
    if (a->buffer_kind[b])
        a->st_out[a->buffer_slot[b]] &= ~(uint32_t)OUT_SHOWN;
    else
        atomic_fetch_and(&a->st_in[a->buffer_slot[b]], ~(uint32_t)IN_SHOWN);
}

static void commit_buffer(struct app *a, int buffer, uint64_t t_src, int generated)
{
    const uint64_t c = a->commits++;
    a->ring[c & 255] = (struct shown){t_src, mf_now_ns(), generated};
    wlp_commit(a->w, buffer, c, 0, a->h_px);
    if (generated)
        a->s.com_g++;
    else
        a->s.com_r++;
}

static void show_real(struct app *a, int slot, uint64_t t_src)
{
    atomic_fetch_or(&a->st_in[slot], IN_SHOWN);
    commit_buffer(a, a->buf_in[slot], t_src, 0);
}

/* Frames from the cast: to the card, or straight to the glass without one. */
static void take_new(struct app *a)
{
    uint32_t head = atomic_load_explicit(&a->q_head, memory_order_relaxed);
    const uint32_t tail = atomic_load_explicit(&a->q_tail, memory_order_acquire);
    for (; head != tail; head++) {
        const struct newframe f = a->q[head % W_MAX_Q];
        atomic_store_explicit(&a->q_head, head + 1, memory_order_release);
        if (a->pass_only) {
            show_real(a, f.slot, f.pts);
            atomic_fetch_and(&a->st_in[f.slot], ~(uint32_t)IN_WRITING);
            continue;
        }
        if (a->pub - a->done >= MF_RING / 2) {
            /* The card has fallen far behind: the frame still reaches the glass, and the next one cannot ask for a mid against a frame the card never saw. */
            show_real(a, f.slot, f.pts);
            atomic_fetch_and(&a->st_in[f.slot], ~(uint32_t)IN_WRITING);
            a->must_skip_mid = 1;
            continue;
        }
        int o = -1;
        for (int i = 0; i < MF_SLOTS && o < 0; i++) {
            const int k = (a->out_turn + i) % MF_SLOTS;
            if (!a->st_out[k])
                o = k;
        }
        const int want_mid = f.gap_ok && !a->must_skip_mid && o >= 0;
        a->must_skip_mid = 0;
        if (want_mid) {
            a->out_turn = (o + 1) % MF_SLOTS;
            a->st_out[o] = OUT_CARD;
        } else {
            a->s.nomid++;
        }
        const uint64_t seq = ++a->pub;
        struct mf_msg *m = &a->l.ctl->msg[seq % MF_RING];
        m->seq = seq;
        m->in_slot = (uint32_t)f.slot;
        m->out_slot = (uint32_t)(o >= 0 ? o : 0);
        m->flags = want_mid ? 0u : (uint32_t)MF_NO_MID;
        atomic_fetch_or(&a->st_in[f.slot], IN_CARD);
        atomic_fetch_and(&a->st_in[f.slot], ~(uint32_t)IN_WRITING);
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        a->l.ctl->frames_pub = seq;
        /* A frame with no mid waits for nothing, when nothing is ahead of it: it reaches the glass now. `late` then says it has been shown. */
        const int now_shown = !want_mid && seq == a->done + 1;
        if (now_shown)
            show_real(a, f.slot, f.pts);
        a->jobs[seq % MF_RING] = (struct job){seq, f.slot, want_mid ? o : -1, f.pts, f.t_in, mf_now_ns(), now_shown};
        a->s.sent++;
    }
}

/* What the card has finished: a generated picture, then the real frame it leads to, each taking one refresh by the fifo. */
static void take_done(struct app *a)
{
    const uint64_t have = a->l.ctl->gen_pub;
    while (a->done < have) {
        const uint64_t seq = ++a->done;
        struct job *j = &a->jobs[seq % MF_RING];
        const struct mf_gen g = a->l.ctl->gen[seq % MF_RING];
        const uint64_t depth = a->commits - a->shown_ok;
        a->s.card_sum += g.us[MF_T_TOTAL];
        a->s.card_n++;
        a->s.card_max = g.us[MF_T_TOTAL] > a->s.card_max ? g.us[MF_T_TOTAL] : a->s.card_max;
        a->s.moved_sum += g.moved;
        if (g.made == 0)
            a->s.scene++;
        if (j->out >= 0) {
            a->st_out[j->out] &= ~(uint32_t)OUT_CARD;
            if (g.made == 1) {
                a->s.made++;
                if (!j->late && depth < 3) {
                    a->st_out[j->out] |= OUT_SHOWN;
                    /* Half way between the two frames' instants. */
                    commit_buffer(a, a->buf_out[j->out], j->pts - a->interval_ns / 2, 1);
                } else {
                    a->s.skip_g++;
                }
            }
        }
        atomic_fetch_and(&a->st_in[j->in], ~(uint32_t)IN_CARD);
        if (!j->late)
            show_real(a, j->in, j->pts);
    }
}

/* A frame the card has held too long reaches the glass without its mid, and the mid, should it come, is dropped. */
static void take_late(struct app *a)
{
    const uint64_t now = mf_now_ns();
    for (uint64_t s = a->done + 1; s <= a->pub; s++) {
        struct job *j = &a->jobs[s % MF_RING];
        if (!j->late && now - j->t_pub > 2 * a->interval_ns) {
            j->late = 1;
            a->s.late++;
            show_real(a, j->in, j->pts);
        }
    }
}

static void print_second(struct app *a, double secs, FILE *out)
{
    struct stats *s = &a->s;
    const uint64_t cast = atomic_exchange(&a->a_cast, 0), dup = atomic_exchange(&a->a_dup, 0), noslot = atomic_exchange(&a->a_noslot, 0);
    fprintf(out, "%6.1f s cast %6.2f dup %6.2f noslot %llu | card sent %6.2f made %6.2f scene %llu nomid %llu late %llu | commit g %llu r %llu skip %llu | glass %6.2f/s (g %6.2f r %6.2f) disc %llu cadence 1:%llu 2:%llu 3:%llu 4+:%llu gapmax %.1f ms | latency ms %.1f mean %.1f..%.1f | card ms %.2f max %.2f moved %.1f\n",
            secs, (double)cast, (double)dup, (unsigned long long)noslot, (double)s->sent, (double)s->made, (unsigned long long)s->scene, (unsigned long long)s->nomid, (unsigned long long)s->late, (unsigned long long)s->com_g, (unsigned long long)s->com_r,
            (unsigned long long)s->skip_g, (double)(s->pres_g + s->pres_r), (double)s->pres_g, (double)s->pres_r, (unsigned long long)s->disc, (unsigned long long)s->cad[1], (unsigned long long)s->cad[2], (unsigned long long)s->cad[3], (unsigned long long)s->cad[4], (double)s->gap_max * 1e-6, s->lat_n ? (double)s->lat_sum / (double)s->lat_n * 1e-6 : 0.0,
            (double)s->lat_min * 1e-6, (double)s->lat_max * 1e-6, s->card_n ? (double)s->card_sum / (double)s->card_n * 1e-3 : 0.0, (double)s->card_max * 1e-3, s->card_n ? (double)s->moved_sum / (double)s->card_n : 0.0);
    fflush(out);
    memset(s, 0, sizeof *s);
}

int main(int argc, char **argv)
{
    static struct app a;
    uint32_t node = 0;
    int tpc = 2, cores = 60, form = 0, seconds = 0, no_excl = 0;
    const char *output = NULL;
    a.w_px = 1366;
    a.h_px = 768;
    a.prev_in = -1;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--node") && i + 1 < argc)
            node = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--output") && i + 1 < argc)
            output = argv[++i];
        else if (!strcmp(argv[i], "--tpc") && i + 1 < argc)
            tpc = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--cores") && i + 1 < argc)
            cores = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--plain"))
            form = 1;
        else if (!strcmp(argv[i], "--pass"))
            a.pass_only = 1;
        else if (!strcmp(argv[i], "--seconds") && i + 1 < argc)
            seconds = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--no-excl"))
            no_excl = 1;
        else {
            fprintf(stderr, "usage: mvexflow --node N --output NAME [--tpc T] [--cores C] [--plain] [--pass] [--seconds S]\n");
            return 2;
        }
    }
    if (!node || !output) {
        fprintf(stderr, "mvexflow: --node and --output are both needed: run through desk cast\n");
        return 2;
    }
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGHUP, on_signal);
    if (mf_link_open(&a.l))
        return 1;
    a.stride_b = plane_stride(a.w_px) * 4;
    if (!a.pass_only) {
        const struct mf_config cfg = {.w = (uint32_t)a.w_px, .h = (uint32_t)a.h_px, .cores = (uint32_t)cores, .tpc = (uint32_t)tpc, .form = (uint32_t)form};
        if (mf_session_start(&a.l, &cfg, 15.0))
            return 1;
        printf("card: %u cores, %u threads, tsc %u MHz, form %s\n", a.l.ctl->card_cores, a.l.ctl->card_threads, a.l.ctl->card_mhz, form ? "plain" : "fused");
    }
    const struct wlp_cb cb = {&a, on_presented, on_discarded, on_released};
    a.w = wlp_open(output, a.w_px, a.h_px, 1, &cb);
    if (!a.w)
        return 1;
    const int mhz = wlp_refresh_mhz(a.w);
    a.interval_ns = 2000000000000ull / (uint64_t)mhz;
    int lw, lh;
    wlp_size(a.w, &lw, &lh);
    printf("overlay on %s: %d x %d logical, %d x %d buffer, refresh %d mHz, cast capped at %d/2000\n", output, lw, lh, a.w_px, a.h_px, mhz, mhz);
    if (wlp_pool(a.w, a.l.fd, MF_PART_OFFSET + MF_PART_BYTES))
        return 1;
    for (int i = 0; i < MF_SLOTS; i++) {
        a.buf_in[i] = wlp_buffer(a.w, MF_PART_OFFSET + mf_in_offset((uint32_t)i) + px0(&a), a.stride_b, 1);
        a.buffer_kind[a.buf_in[i]] = 0;
        a.buffer_slot[a.buf_in[i]] = i;
        a.buf_out[i] = wlp_buffer(a.w, MF_PART_OFFSET + mf_out_offset((uint32_t)i) + px0(&a), a.stride_b, 1);
        a.buffer_kind[a.buf_out[i]] = 1;
        a.buffer_slot[a.buf_out[i]] = i;
    }
    if (!no_excl) {
        char cmd[128];
        snprintf(cmd, sizeof cmd, "scripts/excl.sh on %d >/dev/null", (int)getpid());
        if (system(cmd)) {
            fprintf(stderr, "mvexflow: the capture exclusion failed: refusing to run a feedback loop\n");
            return 1;
        }
    }
    a.ev = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    struct pwc *cast = pwc_open(node, a.w_px, a.h_px, (uint32_t)mhz, 2000, on_frame, &a);
    if (!cast)
        return 1;
    wlp_flush(a.w);
    const uint64_t t_start = mf_now_ns();
    uint64_t t_print = t_start + 1000000000ull;
    int printed_format = 0;
    while (!stop) {
        struct pollfd q[2] = {{wlp_fd(a.w), POLLIN, 0}, {a.ev, POLLIN, 0}};
        /* While a frame is at the card its answer is a word in host memory: looked at often. */
        const struct timespec fast = {0, 100000}, slow = {0, 50000000};
        ppoll(q, 2, a.pub != a.done ? &fast : &slow, NULL);
        if (q[1].revents & POLLIN) {
            uint64_t v;
            if (read(a.ev, &v, 8) != 8)
                v = 0;
        }
        if (wlp_read(a.w) < 0) {
            fprintf(stderr, "mvexflow: the compositor closed the overlay\n");
            break;
        }
        take_new(&a);
        if (!a.pass_only) {
            take_done(&a);
            take_late(&a);
        }
        if (wlp_flush(a.w) < 0)
            break;
        const uint64_t now = mf_now_ns();
        if (now >= t_print) {
            if (!printed_format && pwc_format(cast)[0]) {
                printf("cast: %s\n", pwc_format(cast));
                printed_format = 1;
            }
            print_second(&a, (double)(now - t_start) * 1e-9, stdout);
            t_print += 1000000000ull;
            if (seconds && now - t_start >= (uint64_t)seconds * 1000000000ull)
                break;
        }
    }
    pwc_close(cast);
    if (!no_excl && system("scripts/excl.sh off >/dev/null"))
        fprintf(stderr, "mvexflow: excl.sh off failed\n");
    if (!a.pass_only)
        mf_session_end(&a.l, 5.0);
    wlp_close(a.w);
    mf_link_close(&a.l);
    printf("mvexflow: done\n");
    return 0;
}

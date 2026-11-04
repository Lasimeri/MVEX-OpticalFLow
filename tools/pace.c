/* pace: does this program cost another monitor its frames? An invisible probe: a 32 x 32 fully transparent, click-through surface in a corner of the named output, committed once a refresh under wp_fifo, every commit's instant from wp_presentation. Prints how
 * many pictures followed the last by 1, 2, 3 and more refreshes. Nothing can be seen, and it reads nothing of the screen.
 *
 * pace OUTPUT SECONDS */
#define _GNU_SOURCE
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>

#include "fifo-v1-client.h"
#include "presentation-time-client.h"
#include "viewporter-client.h"
#include "wlr-layer-shell-unstable-v1-client.h"

static struct wl_compositor *comp;
static struct wl_shm *shm;
static struct zwlr_layer_shell_v1 *shell;
static struct wp_presentation *pres;
static struct wp_fifo_manager_v1 *fifom;
static struct wl_output *outs[8];
static char names[8][32];
static int nout, configured, in_flight, discarded;
static uint64_t last_t, n_gap[6], gap_max, refresh_ns;

static void o_geo(void *u, struct wl_output *o, int32_t a, int32_t b, int32_t c, int32_t d, int32_t e, const char *f, const char *g, int32_t h) { (void)u, (void)o, (void)a, (void)b, (void)c, (void)d, (void)e, (void)f, (void)g, (void)h; }
static void o_mode(void *u, struct wl_output *o, uint32_t a, int32_t b, int32_t c, int32_t d) { (void)u, (void)o, (void)a, (void)b, (void)c, (void)d; }
static void o_done(void *u, struct wl_output *o) { (void)u, (void)o; }
static void o_scale(void *u, struct wl_output *o, int32_t a) { (void)u, (void)o, (void)a; }
static void o_name(void *u, struct wl_output *o, const char *n)
{
    (void)o;
    snprintf(names[(intptr_t)u], 32, "%s", n);
}
static void o_desc(void *u, struct wl_output *o, const char *d) { (void)u, (void)o, (void)d; }
static const struct wl_output_listener o_l = {o_geo, o_mode, o_done, o_scale, o_name, o_desc};

static void g_add(void *u, struct wl_registry *r, uint32_t n, const char *i, uint32_t v)
{
    (void)u;
    if (!strcmp(i, wl_compositor_interface.name))
        comp = wl_registry_bind(r, n, &wl_compositor_interface, 4);
    else if (!strcmp(i, wl_shm_interface.name))
        shm = wl_registry_bind(r, n, &wl_shm_interface, 1);
    else if (!strcmp(i, zwlr_layer_shell_v1_interface.name))
        shell = wl_registry_bind(r, n, &zwlr_layer_shell_v1_interface, v < 4 ? v : 4);
    else if (!strcmp(i, wp_presentation_interface.name))
        pres = wl_registry_bind(r, n, &wp_presentation_interface, 1);
    else if (!strcmp(i, wp_fifo_manager_v1_interface.name))
        fifom = wl_registry_bind(r, n, &wp_fifo_manager_v1_interface, 1);
    else if (!strcmp(i, wl_output_interface.name) && nout < 8) {
        outs[nout] = wl_registry_bind(r, n, &wl_output_interface, 4);
        wl_output_add_listener(outs[nout], &o_l, (void *)(intptr_t)nout);
        nout++;
    }
}
static void g_rm(void *u, struct wl_registry *r, uint32_t n) { (void)u, (void)r, (void)n; }
static const struct wl_registry_listener g_l = {g_add, g_rm};

static void ls_cfg(void *u, struct zwlr_layer_surface_v1 *s, uint32_t serial, uint32_t w, uint32_t h)
{
    (void)u, (void)w, (void)h;
    zwlr_layer_surface_v1_ack_configure(s, serial);
    configured = 1;
}
static void ls_close(void *u, struct zwlr_layer_surface_v1 *s) { (void)u, (void)s; }
static const struct zwlr_layer_surface_v1_listener ls_l = {ls_cfg, ls_close};

static void fb_sync(void *u, struct wp_presentation_feedback *f, struct wl_output *o) { (void)u, (void)f, (void)o; }
static void fb_presented(void *u, struct wp_presentation_feedback *f, uint32_t hi, uint32_t lo, uint32_t ns, uint32_t refresh, uint32_t shi, uint32_t slo, uint32_t flags)
{
    (void)u, (void)shi, (void)slo, (void)flags;
    const uint64_t t = (((uint64_t)hi << 32 | lo) * 1000000000ull) + ns;
    refresh_ns = refresh;
    if (last_t && refresh) {
        const uint64_t gap = t - last_t, slots = (gap + refresh / 2) / refresh;
        n_gap[slots > 5 ? 5 : slots]++;
        gap_max = gap > gap_max ? gap : gap_max;
    }
    last_t = t;
    in_flight--;
    wp_presentation_feedback_destroy(f);
}
static void fb_discarded(void *u, struct wp_presentation_feedback *f)
{
    (void)u;
    discarded++;
    in_flight--;
    wp_presentation_feedback_destroy(f);
}
static const struct wp_presentation_feedback_listener fb_l = {fb_sync, fb_presented, fb_discarded};

int main(int argc, char **argv)
{
    if (argc < 3)
        return 2;
    const double seconds = atof(argv[2]);
    struct wl_display *d = wl_display_connect(NULL);
    if (!d)
        return 1;
    wl_registry_add_listener(wl_display_get_registry(d), &g_l, NULL);
    wl_display_roundtrip(d);
    wl_display_roundtrip(d);
    struct wl_output *o = NULL;
    for (int i = 0; i < nout; i++)
        if (!strcmp(names[i], argv[1]))
            o = outs[i];
    if (!o || !comp || !shm || !shell || !pres || !fifom)
        return 1;
    const size_t bytes = 32u * 32u * 4u;
    const int fd = memfd_create("pace", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, (off_t)(bytes * 2)))
        return 1;
    struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, (int32_t)(bytes * 2));
    struct wl_buffer *b[2] = {wl_shm_pool_create_buffer(pool, 0, 32, 32, 128, WL_SHM_FORMAT_ARGB8888), wl_shm_pool_create_buffer(pool, (int32_t)bytes, 32, 32, 128, WL_SHM_FORMAT_ARGB8888)};
    struct wl_surface *s = wl_compositor_create_surface(comp);
    struct wl_region *none = wl_compositor_create_region(comp);
    wl_surface_set_input_region(s, none);
    wl_region_destroy(none);
    struct wp_fifo_v1 *fifo = wp_fifo_manager_v1_get_fifo(fifom, s);
    struct zwlr_layer_surface_v1 *ls = zwlr_layer_shell_v1_get_layer_surface(shell, s, o, ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, "mvexflow-pace");
    zwlr_layer_surface_v1_add_listener(ls, &ls_l, NULL);
    zwlr_layer_surface_v1_set_anchor(ls, ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT);
    zwlr_layer_surface_v1_set_size(ls, 32, 32);
    zwlr_layer_surface_v1_set_exclusive_zone(ls, -1);
    wl_surface_commit(s);
    while (!configured && wl_display_dispatch(d) >= 0) {
    }
    struct timespec t0, t;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    int k = 0;
    for (;;) {
        clock_gettime(CLOCK_MONOTONIC, &t);
        if ((double)(t.tv_sec - t0.tv_sec) + 1e-9 * (double)(t.tv_nsec - t0.tv_nsec) > seconds)
            break;
        while (in_flight < 2) {
            wl_surface_attach(s, b[k++ & 1], 0, 0);
            wl_surface_damage_buffer(s, 0, 0, 32, 32);
            wp_fifo_v1_set_barrier(fifo);
            wp_fifo_v1_wait_barrier(fifo);
            wp_presentation_feedback_add_listener(wp_presentation_feedback(pres, s), &fb_l, NULL);
            wl_surface_commit(s);
            in_flight++;
        }
        wl_display_flush(d);
        struct pollfd q = {wl_display_get_fd(d), POLLIN, 0};
        if (poll(&q, 1, 20) > 0)
            wl_display_dispatch(d);
    }
    const uint64_t total = n_gap[0] + n_gap[1] + n_gap[2] + n_gap[3] + n_gap[4] + n_gap[5];
    printf("%s %.0f s: %.3f pictures/s, refresh %.3f ms; followed the last by 1: %llu, 2: %llu, 3: %llu, 4+: %llu (%.3f %% missed); discarded %d; longest gap %.2f ms\n", argv[1], seconds, (double)total / seconds, (double)refresh_ns * 1e-6, (unsigned long long)n_gap[1], (unsigned long long)n_gap[2],
           (unsigned long long)n_gap[3], (unsigned long long)(n_gap[4] + n_gap[5]), total ? 100.0 * (double)(total - n_gap[1]) / (double)total : 0.0, discarded, (double)gap_max * 1e-6);
    wl_display_disconnect(d);
    return 0;
}

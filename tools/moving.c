/* moving: test content for the monitor under the overlay: my own picture, nothing of the user's. A full-screen layer surface (layer "top", click-through) that redraws every refresh: a panning texture and an opaque box crossing it, whole pixels per
 * refresh, so a 29.895 frames/s cast of it has real motion in every frame.
 *
 * moving OUTPUT SECONDS [PAN_PX_PER_REFRESH] [BOX_PX_PER_REFRESH] */
#define _GNU_SOURCE
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>

#include "viewporter-client.h"
#include "wlr-layer-shell-unstable-v1-client.h"

#define W 1366
#define H 768
#define TW 4096
#define TH 1536

static struct wl_compositor *comp;
static struct wl_shm *shm;
static struct zwlr_layer_shell_v1 *shell;
static struct wp_viewporter *vp;
static struct wl_output *outs[8];
static char names[8][32];
static int nout, configured, lw, lh, frame_due = 1;
static struct wp_viewport *view;

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
    else if (!strcmp(i, wp_viewporter_interface.name))
        vp = wl_registry_bind(r, n, &wp_viewporter_interface, 1);
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
    (void)u;
    zwlr_layer_surface_v1_ack_configure(s, serial);
    lw = (int)w;
    lh = (int)h;
    wp_viewport_set_source(view, wl_fixed_from_int(0), wl_fixed_from_int(0), wl_fixed_from_int(W), wl_fixed_from_int(H));
    wp_viewport_set_destination(view, lw, lh);
    configured = 1;
}
static void ls_close(void *u, struct zwlr_layer_surface_v1 *s) { (void)u, (void)s; }
static const struct zwlr_layer_surface_v1_listener ls_l = {ls_cfg, ls_close};

static void cb_done(void *u, struct wl_callback *c, uint32_t t)
{
    (void)u, (void)t;
    wl_callback_destroy(c);
    frame_due = 1;
}
static const struct wl_callback_listener cb_l = {cb_done};

static uint64_t rng_s = 0x123456789abcdefull;
static uint64_t rng(void)
{
    rng_s ^= rng_s << 13;
    rng_s ^= rng_s >> 7;
    rng_s ^= rng_s << 17;
    return rng_s;
}

int main(int argc, char **argv)
{
    if (argc < 2)
        return 2;
    const double seconds = argc > 2 ? atof(argv[2]) : 10.0;
    const int pan = argc > 3 ? atoi(argv[3]) : 6, boxv = argc > 4 ? atoi(argv[4]) : 18;
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
    if (!o || !comp || !shm || !shell || !vp)
        return 1;
    uint32_t *tex = malloc((size_t)TW * TH * 4u);
    for (int y = 0; y < TH; y++)
        for (int x = 0; x < TW; x++)
            tex[(size_t)y * TW + (size_t)x] = 0xff000000u | (uint32_t)(40 + (x >> 4) % 90) << 16 | (uint32_t)(60 + (y >> 3) % 90) << 8 | (uint32_t)(90 + ((x + y) >> 5) % 90);
    for (int n = 0; n < 1500; n++) {
        const int w = 8 + (int)(rng() % 200), h = 6 + (int)(rng() % 70), x0 = (int)(rng() % (TW - (uint64_t)w)), y0 = (int)(rng() % (TH - (uint64_t)h));
        const uint32_t c = 0xff000000u | (uint32_t)(rng() & 0xffffff);
        for (int y = y0; y < y0 + h; y++)
            for (int x = x0; x < x0 + w; x++)
                tex[(size_t)y * TW + (size_t)x] = c;
    }
    for (int n = 0; n < 50000; n++) {
        const int w = 1 + (int)(rng() % 6), h = 2 + (int)(rng() % 8), x0 = (int)(rng() % (TW - 8)), y0 = (int)(rng() % (TH - 12));
        const uint32_t c = (rng() & 1) ? 0xff101010u : 0xffe8e8e8u;
        for (int y = y0; y < y0 + h; y++)
            for (int x = x0; x < x0 + w; x++)
                tex[(size_t)y * TW + (size_t)x] = c;
    }
    const size_t one = (size_t)W * H * 4u;
    const int fd = memfd_create("moving", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, (off_t)(one * 3)))
        return 1;
    uint32_t *px = mmap(NULL, one * 3, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, (int32_t)(one * 3));
    struct wl_buffer *b[3];
    for (int i = 0; i < 3; i++)
        b[i] = wl_shm_pool_create_buffer(pool, (int32_t)(one * (size_t)i), W, H, W * 4, WL_SHM_FORMAT_XRGB8888);
    struct wl_surface *s = wl_compositor_create_surface(comp);
    view = wp_viewporter_get_viewport(vp, s);
    struct wl_region *none = wl_compositor_create_region(comp);
    wl_surface_set_input_region(s, none);
    wl_region_destroy(none);
    struct zwlr_layer_surface_v1 *ls = zwlr_layer_shell_v1_get_layer_surface(shell, s, o, ZWLR_LAYER_SHELL_V1_LAYER_TOP, "mvexflow-test");
    zwlr_layer_surface_v1_add_listener(ls, &ls_l, NULL);
    zwlr_layer_surface_v1_set_anchor(ls, 15);
    zwlr_layer_surface_v1_set_exclusive_zone(ls, -1);
    wl_surface_commit(s);
    while (!configured && wl_display_dispatch(d) >= 0) {
    }
    struct timespec t0, t;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    int frame = 0;
    for (;;) {
        clock_gettime(CLOCK_MONOTONIC, &t);
        const double el = (double)(t.tv_sec - t0.tv_sec) + 1e-9 * (double)(t.tv_nsec - t0.tv_nsec);
        if (el > seconds)
            break;
        if (frame_due) {
            frame_due = 0;
            const int bi = frame % 3;
            uint32_t *p = px + (size_t)bi * (size_t)W * H;
            const int ox = (frame * pan) % (TW - W);
            for (int y = 0; y < H; y++)
                memcpy(p + (size_t)y * W, tex + (size_t)y * TW + (size_t)ox, W * 4u);
            const int bx = (frame * boxv) % (W - 200), by = 200 + (frame * 2) % 300;
            for (int y = by; y < by + 200; y++)
                memcpy(p + (size_t)y * W + (size_t)bx, tex + (size_t)(y + 700) * TW + 1000u, 200u * 4u);
            wl_surface_attach(s, b[bi], 0, 0);
            wl_surface_damage_buffer(s, 0, 0, W, H);
            wl_callback_add_listener(wl_surface_frame(s), &cb_l, NULL);
            wl_surface_commit(s);
            frame++;
        }
        wl_display_flush(d);
        struct pollfd q = {wl_display_get_fd(d), POLLIN, 0};
        poll(&q, 1, 5);
        if (q.revents & POLLIN)
            wl_display_dispatch(d);
        else
            wl_display_dispatch_pending(d);
    }
    printf("moving: %d frames in %.3f s = %.3f /s\n", frame, seconds, (double)frame / seconds);
    wl_display_disconnect(d);
    return 0;
}

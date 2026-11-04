/* wlp.c: see wlp.h. */
#define _GNU_SOURCE
#include "wlp.h"

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <wayland-client.h>

#include "fifo-v1-client.h"
#include "presentation-time-client.h"
#include "viewporter-client.h"
#include "wlr-layer-shell-unstable-v1-client.h"

struct wout {
    struct wl_output *o;
    char name[64];
    int mhz;
};

struct wbuf {
    struct wlp *p;
    struct wl_buffer *b;
    int index;
};

struct fbk {
    struct wlp *p;
    uint64_t cookie;
};

struct wlp {
    struct wl_display *d;
    struct wl_registry *reg;
    struct wl_compositor *comp;
    struct wl_shm *shm;
    struct zwlr_layer_shell_v1 *shell;
    struct wp_viewporter *vp;
    struct wp_presentation *pres;
    struct wp_fifo_manager_v1 *fifom;
    struct wout out[8];
    int nout, chosen;
    struct wl_surface *s;
    struct zwlr_layer_surface_v1 *ls;
    struct wp_viewport *view;
    struct wp_fifo_v1 *fifo;
    struct wl_shm_pool *pool;
    struct wbuf buf[WLP_MAX_BUFFERS];
    int nbuf, w, h, lw, lh, configured, closed;
    uint32_t pres_clock;
    struct wlp_cb cb;
};

static void out_geometry(void *u, struct wl_output *o, int32_t x, int32_t y, int32_t pw, int32_t ph, int32_t sp, const char *mk, const char *md, int32_t tr)
{
    (void)u, (void)o, (void)x, (void)y, (void)pw, (void)ph, (void)sp, (void)mk, (void)md, (void)tr;
}
static void out_mode(void *u, struct wl_output *o, uint32_t flags, int32_t w, int32_t h, int32_t mhz)
{
    (void)o, (void)w, (void)h;
    struct wout *e = u;
    if (flags & WL_OUTPUT_MODE_CURRENT)
        e->mhz = mhz;
}
static void out_done(void *u, struct wl_output *o) { (void)u, (void)o; }
static void out_scale(void *u, struct wl_output *o, int32_t f) { (void)u, (void)o, (void)f; }
static void out_name(void *u, struct wl_output *o, const char *name)
{
    (void)o;
    struct wout *e = u;
    snprintf(e->name, sizeof e->name, "%s", name);
}
static void out_desc(void *u, struct wl_output *o, const char *t) { (void)u, (void)o, (void)t; }
static const struct wl_output_listener out_l = {out_geometry, out_mode, out_done, out_scale, out_name, out_desc};

static void pres_clock(void *u, struct wp_presentation *p, uint32_t id)
{
    (void)p;
    ((struct wlp *)u)->pres_clock = id;
}
static const struct wp_presentation_listener pres_l = {pres_clock};

static void reg_global(void *u, struct wl_registry *r, uint32_t name, const char *iface, uint32_t ver)
{
    struct wlp *p = u;
    if (!strcmp(iface, wl_compositor_interface.name))
        p->comp = wl_registry_bind(r, name, &wl_compositor_interface, 4);
    else if (!strcmp(iface, wl_shm_interface.name))
        p->shm = wl_registry_bind(r, name, &wl_shm_interface, 1);
    else if (!strcmp(iface, zwlr_layer_shell_v1_interface.name))
        p->shell = wl_registry_bind(r, name, &zwlr_layer_shell_v1_interface, ver < 4 ? ver : 4);
    else if (!strcmp(iface, wp_viewporter_interface.name))
        p->vp = wl_registry_bind(r, name, &wp_viewporter_interface, 1);
    else if (!strcmp(iface, wp_presentation_interface.name)) {
        p->pres = wl_registry_bind(r, name, &wp_presentation_interface, 1);
        wp_presentation_add_listener(p->pres, &pres_l, p);
    } else if (!strcmp(iface, wp_fifo_manager_v1_interface.name))
        p->fifom = wl_registry_bind(r, name, &wp_fifo_manager_v1_interface, 1);
    else if (!strcmp(iface, wl_output_interface.name) && p->nout < 8) {
        struct wout *e = &p->out[p->nout++];
        e->o = wl_registry_bind(r, name, &wl_output_interface, 4);
        wl_output_add_listener(e->o, &out_l, e);
    }
}
static void reg_remove(void *u, struct wl_registry *r, uint32_t name) { (void)u, (void)r, (void)name; }
static const struct wl_registry_listener reg_l = {reg_global, reg_remove};

static void ls_configure(void *u, struct zwlr_layer_surface_v1 *ls, uint32_t serial, uint32_t w, uint32_t h)
{
    struct wlp *p = u;
    zwlr_layer_surface_v1_ack_configure(ls, serial);
    p->lw = (int)w;
    p->lh = (int)h;
    p->configured = 1;
    wp_viewport_set_source(p->view, wl_fixed_from_int(0), wl_fixed_from_int(0), wl_fixed_from_int(p->w), wl_fixed_from_int(p->h));
    wp_viewport_set_destination(p->view, (int)w, (int)h);
}
static void ls_closed(void *u, struct zwlr_layer_surface_v1 *ls)
{
    (void)ls;
    ((struct wlp *)u)->closed = 1;
}
static const struct zwlr_layer_surface_v1_listener ls_l = {ls_configure, ls_closed};

static void buf_release(void *u, struct wl_buffer *b)
{
    (void)b;
    struct wbuf *e = u;
    if (e->p->cb.released)
        e->p->cb.released(e->p->cb.user, e->index);
}
static const struct wl_buffer_listener buf_l = {buf_release};

static void fb_sync(void *u, struct wp_presentation_feedback *f, struct wl_output *o) { (void)u, (void)f, (void)o; }
static void fb_presented(void *u, struct wp_presentation_feedback *f, uint32_t hi, uint32_t lo, uint32_t ns, uint32_t refresh, uint32_t shi, uint32_t slo, uint32_t flags)
{
    struct fbk *k = u;
    if (k->p->cb.presented)
        k->p->cb.presented(k->p->cb.user, k->cookie, (((uint64_t)hi << 32 | lo) * 1000000000ull) + ns, refresh, (uint64_t)shi << 32 | slo, flags);
    wp_presentation_feedback_destroy(f);
    free(k);
}
static void fb_discarded(void *u, struct wp_presentation_feedback *f)
{
    struct fbk *k = u;
    if (k->p->cb.discarded)
        k->p->cb.discarded(k->p->cb.user, k->cookie);
    wp_presentation_feedback_destroy(f);
    free(k);
}
static const struct wp_presentation_feedback_listener fb_l = {fb_sync, fb_presented, fb_discarded};

struct wlp *wlp_open(const char *output, int w, int h, int use_fifo, const struct wlp_cb *cb)
{
    struct wlp *p = calloc(1, sizeof *p);
    p->w = w;
    p->h = h;
    p->chosen = -1;
    p->cb = *cb;
    p->d = wl_display_connect(NULL);
    if (!p->d) {
        fprintf(stderr, "wlp: no Wayland display\n");
        return NULL;
    }
    p->reg = wl_display_get_registry(p->d);
    wl_registry_add_listener(p->reg, &reg_l, p);
    wl_display_roundtrip(p->d);
    wl_display_roundtrip(p->d);
    for (int i = 0; i < p->nout; i++)
        if (!strcmp(p->out[i].name, output))
            p->chosen = i;
    if (p->chosen < 0 || !p->comp || !p->shm || !p->shell || !p->vp || !p->pres) {
        fprintf(stderr, "wlp: output %s %s; compositor %d shm %d layer shell %d viewporter %d presentation %d\n", output, p->chosen < 0 ? "absent" : "found", !!p->comp, !!p->shm, !!p->shell, !!p->vp, !!p->pres);
        return NULL;
    }
    if (use_fifo && !p->fifom) {
        fprintf(stderr, "wlp: no wp_fifo_manager_v1\n");
        return NULL;
    }
    if (p->pres_clock != CLOCK_MONOTONIC) {
        fprintf(stderr, "wlp: the compositor's clock is %u, not CLOCK_MONOTONIC\n", p->pres_clock);
        return NULL;
    }
    p->s = wl_compositor_create_surface(p->comp);
    p->view = wp_viewporter_get_viewport(p->vp, p->s);
    if (use_fifo)
        p->fifo = wp_fifo_manager_v1_get_fifo(p->fifom, p->s);
    /* Clicks and the wheel go to whatever lies under it. */
    struct wl_region *none = wl_compositor_create_region(p->comp);
    wl_surface_set_input_region(p->s, none);
    wl_region_destroy(none);
    p->ls = zwlr_layer_shell_v1_get_layer_surface(p->shell, p->s, p->out[p->chosen].o, ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, "mvexflow");
    zwlr_layer_surface_v1_add_listener(p->ls, &ls_l, p);
    zwlr_layer_surface_v1_set_anchor(p->ls, ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM | ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
    zwlr_layer_surface_v1_set_exclusive_zone(p->ls, -1);
    zwlr_layer_surface_v1_set_keyboard_interactivity(p->ls, ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE);
    wl_surface_commit(p->s);
    while (!p->configured && !p->closed && wl_display_dispatch(p->d) >= 0) {
    }
    if (!p->configured) {
        fprintf(stderr, "wlp: the compositor refused the surface\n");
        return NULL;
    }
    return p;
}

void wlp_close(struct wlp *p)
{
    if (!p)
        return;
    if (p->ls)
        zwlr_layer_surface_v1_destroy(p->ls);
    if (p->s)
        wl_surface_destroy(p->s);
    wl_display_flush(p->d);
    wl_display_disconnect(p->d);
    free(p);
}

int wlp_pool(struct wlp *p, int fd, uint64_t bytes)
{
    if (bytes > 0x7fffffffull)
        return -1;
    p->pool = wl_shm_create_pool(p->shm, fd, (int32_t)bytes);
    return p->pool ? 0 : -1;
}

int wlp_buffer(struct wlp *p, uint64_t offset, int stride, int alpha)
{
    if (p->nbuf >= WLP_MAX_BUFFERS || !p->pool)
        return -1;
    struct wbuf *e = &p->buf[p->nbuf];
    e->p = p;
    e->index = p->nbuf;
    e->b = wl_shm_pool_create_buffer(p->pool, (int32_t)offset, p->w, p->h, stride, alpha ? WL_SHM_FORMAT_ARGB8888 : WL_SHM_FORMAT_XRGB8888);
    wl_buffer_add_listener(e->b, &buf_l, e);
    return p->nbuf++;
}

void wlp_commit(struct wlp *p, int buffer, uint64_t cookie, int y0, int y1)
{
    wl_surface_attach(p->s, p->buf[buffer].b, 0, 0);
    wl_surface_damage_buffer(p->s, 0, y0, p->w, y1 - y0);
    if (p->fifo) {
        wp_fifo_v1_set_barrier(p->fifo);
        wp_fifo_v1_wait_barrier(p->fifo);
    }
    struct fbk *k = malloc(sizeof *k);
    k->p = p;
    k->cookie = cookie;
    wp_presentation_feedback_add_listener(wp_presentation_feedback(p->pres, p->s), &fb_l, k);
    wl_surface_commit(p->s);
}

int wlp_fd(const struct wlp *p) { return wl_display_get_fd(p->d); }

int wlp_flush(struct wlp *p)
{
    if (wl_display_flush(p->d) < 0 && errno != EAGAIN)
        return -1;
    return p->closed ? -1 : 0;
}

int wlp_read(struct wlp *p)
{
    while (wl_display_prepare_read(p->d) != 0)
        if (wl_display_dispatch_pending(p->d) < 0)
            return -1;
    struct pollfd q = {wl_display_get_fd(p->d), POLLIN, 0};
    if (poll(&q, 1, 0) > 0 && (q.revents & POLLIN)) {
        if (wl_display_read_events(p->d) < 0)
            return -1;
    } else {
        wl_display_cancel_read(p->d);
    }
    if (wl_display_dispatch_pending(p->d) < 0)
        return -1;
    return p->closed ? -1 : 0;
}

void wlp_size(const struct wlp *p, int *w, int *h)
{
    *w = p->lw;
    *h = p->lh;
}

int wlp_refresh_mhz(const struct wlp *p) { return p->out[p->chosen].mhz; }

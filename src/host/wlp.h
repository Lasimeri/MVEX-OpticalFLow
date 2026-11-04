/* wlp.h: the picture on the monitor. A click-through wlr layer surface over one named output, buffers from a wl_shm pool the caller owns (so a card's DMA target can be shown with no copy),
 * one commit per refresh by wp_fifo, every commit's fate reported by wp_presentation. */
#ifndef MF_WLP_H
#define MF_WLP_H

#include <stdint.h>

#define WLP_MAX_BUFFERS 16

struct wlp;

struct wlp_cb {
    void *user;
    /* A commit reached the glass: the instant of its scanout on CLOCK_MONOTONIC (the compositor's clock, checked at open), the output's refresh period in ns, the vblank counter. */
    void (*presented)(void *user, uint64_t cookie, uint64_t t_ns, uint32_t refresh_ns, uint64_t vblank, uint32_t flags);
    /* A commit was replaced before it was shown. */
    void (*discarded)(void *user, uint64_t cookie);
    /* The compositor let go of a buffer. */
    void (*released)(void *user, int buffer);
};

/* Connects, finds the output by name, maps a full-screen surface in the overlay layer with an empty input region. w, h: the buffers' pixels. 0 fifo: no barrier. NULL with a message. */
struct wlp *wlp_open(const char *output, int w, int h, int use_fifo, const struct wlp_cb *cb);
void wlp_close(struct wlp *p);

/* The pool: fd is not kept open by the compositor beyond its own dup. bytes fits an int32. */
int wlp_pool(struct wlp *p, int fd, uint64_t bytes);
/* A buffer at offset in the pool, stride in bytes. Its index, or -1. alpha: ARGB8888, so the compositor keeps painting what lies under it; else XRGB8888. */
int wlp_buffer(struct wlp *p, uint64_t offset, int stride, int alpha);

/* Shows the buffer at the next free refresh. Rows y0 .. y1 are declared changed. */
void wlp_commit(struct wlp *p, int buffer, uint64_t cookie, int y0, int y1);

int wlp_fd(const struct wlp *p);
/* Sends what is queued. -1 when the connection has gone. */
int wlp_flush(struct wlp *p);
/* Reads and handles what is waiting, without blocking. -1 when the connection has gone. */
int wlp_read(struct wlp *p);
/* Handles events until the surface has its size; logical size out. */
void wlp_size(const struct wlp *p, int *w, int *h);
/* The refresh of the output, mHz, as the mode says. */
int wlp_refresh_mhz(const struct wlp *p);

#endif

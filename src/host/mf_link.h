/* mf_link.h: the host's view of the part: the control area and the slots. */
#ifndef MF_LINK_H
#define MF_LINK_H

#include <stdint.h>
#include <time.h>

#include "mf_proto.h"

#define MF_REGION_PATH "/dev/shm/phi-hostmem"

struct mf_link {
    int fd;
    uint8_t *part;
    struct mf_ctl *ctl;
};

/* Maps the part. 0, or -1 with a message. fd stays open: it can feed a wl_shm pool. */
int mf_link_open(struct mf_link *l);
void mf_link_close(struct mf_link *l);

static inline uint64_t mf_now_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

/* Publishes a config and waits for the card's answer. 0, or -1 with a message. */
int mf_session_start(struct mf_link *l, const struct mf_config *cfg, double timeout_s);
/* Ends it and waits for the card to leave it. */
void mf_session_end(struct mf_link *l, double timeout_s);

#endif

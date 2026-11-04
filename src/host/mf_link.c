/* mf_link.c: see mf_link.h. */
#define _GNU_SOURCE
#include "mf_link.h"

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

int mf_link_open(struct mf_link *l)
{
    l->fd = open(MF_REGION_PATH, O_RDWR | O_CLOEXEC);
    if (l->fd < 0) {
        perror("mf_link: " MF_REGION_PATH);
        return -1;
    }
    void *p = mmap(NULL, (size_t)MF_PART_BYTES, PROT_READ | PROT_WRITE, MAP_SHARED, l->fd, (off_t)MF_PART_OFFSET);
    if (p == MAP_FAILED) {
        perror("mf_link: mmap");
        return -1;
    }
    l->part = p;
    l->ctl = p;
    return 0;
}

void mf_link_close(struct mf_link *l)
{
    munmap(l->part, (size_t)MF_PART_BYTES);
    close(l->fd);
}

static void nap_ms(void)
{
    struct timespec t = {0, 1000000};
    nanosleep(&t, NULL);
}

int mf_session_start(struct mf_link *l, const struct mf_config *cfg, double timeout_s)
{
    struct mf_ctl *c = l->ctl;
    /* A session still open belongs to a program that died without ending it. */
    if ((int64_t)c->card_session > 0)
        mf_session_end(l, 3.0);
    c->magic = MF_MAGIC;
    c->config = *cfg;
    c->config.format = mf_format_hash();
    c->frames_pub = 0;
    c->gen_pub = 0;
    c->mid_pub = 0;
    c->session_end = 0;
    c->quit = 0;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    const uint64_t seq = c->session_seq + 1;
    c->card_session = 0;
    c->session_seq = seq;
    const uint64_t t0 = mf_now_ns();
    while (c->card_session != seq) {
        if (c->card_session == (uint64_t)-(int64_t)seq) {
            fprintf(stderr, "mf_link: the card refused the session: its format %016llx, this program's %016llx (see /tmp/mvexflow/mvexflowd.log on the card)\n", (unsigned long long)c->card_format, (unsigned long long)mf_format_hash());
            return -1;
        }
        if ((double)(mf_now_ns() - t0) * 1e-9 > timeout_s) {
            fprintf(stderr, "mf_link: no answer from mvexflowd after %.0f s: is it running on the card?\n", timeout_s);
            return -1;
        }
        nap_ms();
    }
    return 0;
}

void mf_session_end(struct mf_link *l, double timeout_s)
{
    l->ctl->session_end = 1;
    const uint64_t t0 = mf_now_ns();
    while (l->ctl->card_session != 0 && (double)(mf_now_ns() - t0) * 1e-9 <= timeout_s)
        nap_ms();
}

/* mvexflowd: the card's program. Idle, one thread naps on the session word in host memory and no other thread exists. A session: the team, the arena, then for every frame the host announces: DMA in, luma and pyramid, and when a mid frame is
 * asked the flow and the picture, DMA out, the answer. One frame at a time on the team's first thread: at 29.895 frames a second there is no need for more, and it keeps the code plain.
 *
 * Started detached by scripts/mvexflow.sh; stopped by the quit word (host: mf_quit). */
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "arena.h"
#include "flow.h"
#include "mf_proto.h"
#include "team.h"
#include "topo.h"
#include "vt_dma.h"
#include "vt_util.h"

#define NAP_NS 100000

static void nap(void)
{
    struct timespec t = {0, NAP_NS};
    nanosleep(&t, NULL);
}

/* A range of the arena against contiguous addresses of the region, cut at huge-page edges and at the engine's largest copy. After Intel-Phi-VisionTrack session_track.c. */
static uint64_t copy_range(struct vt_dma *d, const struct arena *a, void *p, uint64_t host_addr, uint64_t bytes, int to_card)
{
    uint64_t last = 0, off = 0;
    while (off < bytes) {
        uint8_t *v = (uint8_t *)p + off;
        const uint64_t in_page = ARENA_PAGE - ((uint64_t)(v - a->base) % ARENA_PAGE);
        uint64_t n = bytes - off < in_page ? bytes - off : in_page;
        if (n > VT_DMA_MAX)
            n = VT_DMA_MAX;
        const uint64_t card = arena_phys(a, v);
        last = to_card ? vt_dma_copy(d, host_addr + off, card, (uint32_t)n, 0, 0) : vt_dma_copy(d, card, host_addr + off, (uint32_t)n, 0, 0);
        if (!last)
            return 0;
        off += n;
    }
    return last;
}

static void team_runner(void *ctx, flow_fn fn, void *arg) { team_run(ctx, fn, arg); }

static uint64_t measure_khz(void)
{
    struct timespec a, b, s = {0, 20000000};
    clock_gettime(CLOCK_MONOTONIC, &a);
    const uint64_t t0 = vt_tsc();
    nanosleep(&s, NULL);
    const uint64_t t1 = vt_tsc();
    clock_gettime(CLOCK_MONOTONIC, &b);
    const uint64_t ns = (uint64_t)(b.tv_sec - a.tv_sec) * 1000000000ull + (uint64_t)b.tv_nsec - (uint64_t)a.tv_nsec;
    return (t1 - t0) * 1000000ull / ns;
}

/* The team's threads: tpc on each usable core, then, for a core count above the usable ones (60 x 2 by the user's order of 11-04), the threads of a block poller's core that no poller is pinned to. CPU 0's core is never entered. Returns the
 * thread count, -1 when it cannot be done. */
static int select_threads(const struct topo *t, int cores, int tpc, int *cpus, int *core_of)
{
    int n = 0, taken = 0;
    for (int pass = 0; pass < 2 && taken < cores; pass++)
        for (int c = 0; c < t->ncores && taken < cores; c++) {
            const struct topo_core *k = &t->core[c];
            if (pass == 0 ? k->reserved != 0 : k->reserved != 2)
                continue;
            int got = 0;
            for (int i = 0; i < k->ncpus && got < tpc; i++) {
                int poller = 0;
                for (int p = 0; p < t->npollers; p++)
                    poller |= t->poller_cpu[p] == k->cpu[i];
                if (poller)
                    continue;
                cpus[n] = k->cpu[i];
                core_of[n] = taken;
                n++;
                got++;
            }
            if (got < tpc)
                return -1;
            taken++;
        }
    return taken == cores ? n : -1;
}

static int serve(struct mf_ctl *ctl, struct vt_dma *dma, const struct topo *topo, uint64_t session)
{
    struct mf_config cfg;
    memcpy(&cfg, &ctl->config, sizeof cfg);
    const int w = (int)cfg.w, h = (int)cfg.h;
    if (bgra_bytes(w, h) > MF_SLOT_BYTES || w < 64 || h < 64) {
        fprintf(stderr, "mvexflowd: %d x %d does not fit a slot\n", w, h);
        return -1;
    }
    int cpus[TOPO_MAX_CPUS], core_of[TOPO_MAX_CPUS];
    const int cores = cfg.cores ? (int)cfg.cores : 60;
    const int n = select_threads(topo, cores, (int)cfg.tpc, cpus, core_of);
    if (n < 1) {
        fprintf(stderr, "mvexflowd: no %d cores x %u threads\n", cores, cfg.tpc);
        return -1;
    }
    struct arena a;
    if (arena_open(&a, flow_bytes(w, h, (int)cfg.form) + 2u * ARENA_PAGE))
        return -1;
    struct mf_gen *gm = arena_take(&a, 4096, 4096);
    void *mem = arena_take(&a, flow_bytes(w, h, (int)cfg.form), 4096);
    struct team *team = team_create(cpus, core_of, n);
    static struct flow f __attribute__((aligned(64)));
    if (!gm || !mem || !team || flow_init(&f, mem, w, h, (int)cfg.form, team_runner, team)) {
        fprintf(stderr, "mvexflowd: session memory or team\n");
        return -1;
    }
    const uint64_t khz = measure_khz();
    ctl->card_threads = (uint32_t)n;
    ctl->card_cores = (uint32_t)cores;
    ctl->card_mhz = (uint32_t)(khz / 1000u);
    printf("mvexflowd: session %llu: %d x %d, %d cores, %d threads, form %u, %d levels, tsc %llu kHz (card)\n", (unsigned long long)session, w, h, cores, n, cfg.form, f.levels, (unsigned long long)khz);
    const uint64_t part = dma->region_addr + MF_PART_OFFSET;
    const uint64_t frame_bytes = bgra_bytes(w, h);
    /* A picture goes back whole rows, padding columns included, in the slot's own layout. */
    const uint64_t out_skip = (uint64_t)f.out.stride * VT_PAD_Y * 4u, out_bytes = (uint64_t)f.out.stride * (uint64_t)h * 4u;
    ctl->card_session = session;
    uint64_t seen = 0;
    /* A still monitor costs the card nothing: after a second without a frame the team is gone and one thread naps; the next frame, which asks for no mid after a pause, brings it back. */
    const uint64_t idle_cycles = khz * 1000ull;
    uint64_t last_frame = vt_tsc(), ups = 0, up_cycles = 0, up_max = 0;
    int rc = 0;
    while (!ctl->session_end && !ctl->quit) {
        if (ctl->frames_pub <= seen) {
            if (team && vt_tsc() - last_frame > idle_cycles) {
                team_destroy(team);
                team = NULL;
                f.run_ctx = NULL;
            }
            nap();
            continue;
        }
        if (!team) {
            const uint64_t c0 = vt_tsc();
            team = team_create(cpus, core_of, n);
            if (!team) {
                fprintf(stderr, "mvexflowd: no team\n");
                rc = -1;
                break;
            }
            f.run_ctx = team;
            const uint64_t took = vt_tsc() - c0;
            ups++;
            up_cycles += took;
            up_max = took > up_max ? took : up_max;
        }
        seen++;
        const struct mf_msg *m = &ctl->msg[seen % MF_RING];
        const uint32_t in_slot = m->in_slot, out_slot = m->out_slot, flags = m->flags;
        const uint64_t seq = m->seq;
        const uint64_t t0 = vt_tsc();
        uint64_t last = copy_range(dma, &a, bgra_base(flow_next_frame(&f)), part + mf_in_offset(in_slot), frame_bytes, 1);
        if (!last || vt_dma_wait(dma, last, 5.0)) {
            fprintf(stderr, "mvexflowd: frame DMA failed\n");
            rc = -1;
            break;
        }
        const uint64_t t1 = vt_tsc();
        flow_land(&f);
        int made = -1;
        if (!(flags & MF_NO_MID))
            made = flow_make(&f);
        const uint64_t t2 = vt_tsc();
        last = 0;
        if (made == 1) {
            last = copy_range(dma, &a, (char *)bgra_base(&f.out) + out_skip, part + mf_out_offset(out_slot) + out_skip, out_bytes, 0);
            if (!last || vt_dma_wait(dma, last, 5.0)) {
                fprintf(stderr, "mvexflowd: picture DMA failed\n");
                rc = -1;
                break;
            }
        }
        const uint64_t t3 = vt_tsc();
#define US(c) ((uint32_t)((c) * 1000ull / khz))
        memset(gm, 0, sizeof *gm);
        gm->seq = seq;
        gm->out_slot = out_slot;
        gm->made = made;
        gm->moved = made == 1 ? f.moved : 0;
        gm->us[MF_T_DMA_IN] = US(t1 - t0);
        gm->us[MF_T_LAND] = US(f.cycles[FLOW_LAND]);
        gm->us[MF_T_PYR] = US(f.cycles[FLOW_PYR]);
        gm->us[MF_T_HIST] = US(f.cycles[FLOW_HIST]);
        gm->us[MF_T_SEARCH] = US(f.cycles[FLOW_SEARCH]);
        gm->us[MF_T_FILTER] = US(f.cycles[FLOW_FILTER]);
        gm->us[MF_T_UP] = US(f.cycles[FLOW_UP]);
        gm->us[MF_T_MID] = US(f.cycles[FLOW_MID]);
        gm->us[MF_T_BLEND] = US(f.cycles[FLOW_BLEND]);
        gm->us[MF_T_DMA_OUT] = US(t3 - t2);
        gm->us[MF_T_TOTAL] = US(t3 - t0);
#undef US
        last = vt_dma_copy(dma, arena_phys(&a, gm), part + offsetof(struct mf_ctl, gen) + (seen % MF_RING) * sizeof(struct mf_gen), sizeof *gm, part + offsetof(struct mf_ctl, gen_pub), seen);
        if (!last || vt_dma_wait(dma, last, 5.0)) {
            fprintf(stderr, "mvexflowd: answer DMA failed\n");
            rc = -1;
            break;
        }
        last_frame = vt_tsc();
    }
    printf("mvexflowd: session %llu served %llu frames; team brought back %llu times, us mean %llu max %llu (card)\n", (unsigned long long)session, (unsigned long long)seen, (unsigned long long)ups, (unsigned long long)(ups ? up_cycles * 1000ull / khz / ups : 0), (unsigned long long)(up_max * 1000ull / khz));
    if (team)
        team_destroy(team);
    arena_close(&a);
    return rc;
}

/* The region's swap guard, as vtrackd's: the part must not be what /dev/phiblk1 swaps on without an offset. */
static int region_is_swap(void)
{
    FILE *f = fopen("/proc/swaps", "r");
    if (!f)
        return 0;
    char line[256];
    int bad = 0;
    while (fgets(line, sizeof line, f))
        if (strstr(line, "/dev/phiblk1"))
            bad = 1;
    fclose(f);
    return bad;
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    setvbuf(stderr, NULL, _IOLBF, 0);
    if (region_is_swap()) {
        fprintf(stderr, "mvexflowd: /dev/phiblk1 is swap: the region is not free; refused\n");
        return 3;
    }
    const int fd = open("/dev/phihost", O_RDWR);
    if (fd < 0) {
        perror("mvexflowd: /dev/phihost");
        return 3;
    }
    struct mf_ctl *ctl = mmap(NULL, (size_t)MF_IN_OFFSET, PROT_READ | PROT_WRITE, MAP_SHARED, fd, (off_t)MF_PART_OFFSET);
    close(fd);
    if (ctl == MAP_FAILED) {
        perror("mvexflowd: mmap /dev/phihost");
        return 3;
    }
    struct arena dp;
    if (arena_open(&dp, ARENA_PAGE))
        return 3;
    void *ring = arena_take(&dp, 8192, 4096);
    struct vt_dma dma;
    if (vt_dma_open(&dma, ring, arena_phys(&dp, ring)))
        return 3;
    struct topo topo;
    if (topo_discover(&topo))
        return 3;
    const uint64_t format = mf_format_hash();
    printf("mvexflowd: DMA channel %d, region at card address 0x%llx, %llu MiB; %d cpus, %d cores, %d usable; format %016llx (card)\n", dma.chan, (unsigned long long)dma.region_addr, (unsigned long long)(dma.region_bytes >> 20), topo.ncpus, topo.ncores, topo.nusable,
           (unsigned long long)format);
    uint64_t seen = ctl->session_seq;
    ctl->card_session = 0;
    ctl->quit = 0;
    ctl->card_format = format;
    while (!ctl->quit) {
        const uint64_t want = ctl->session_seq;
        if (want == seen) {
            struct timespec t = {0, 2000000};
            nanosleep(&t, NULL);
            continue;
        }
        seen = want;
        if (ctl->magic != MF_MAGIC || ctl->config.format != format) {
            fprintf(stderr, "mvexflowd: refused: format %016llx, mine %016llx\n", (unsigned long long)ctl->config.format, (unsigned long long)format);
            ctl->card_session = (uint64_t)-(int64_t)want;
            continue;
        }
        const int rc = serve(ctl, &dma, &topo, want);
        printf("mvexflowd: session %llu ended rc %d (card)\n", (unsigned long long)want, rc);
        ctl->card_session = rc == 0 ? 0 : (uint64_t)-(int64_t)want;
    }
    vt_dma_close(&dma);
    printf("mvexflowd: quit (card)\n");
    return 0;
}

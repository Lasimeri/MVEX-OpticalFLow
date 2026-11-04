/* mf_proto.h: the contract between the host and the card program, in one part of the shared region (/dev/shm/phi-hostmem on the host, /dev/phihost uncached on the card; the same plumbing as Intel-Phi-VisionTrack's vt_proto.h, with its
 * own part, its own magic, its own format hash). The card reads a few words of it uncached and moves pictures by its own DMA channel; words it owes the host arrive by DMA status descriptors, after the data they announce.
 *
 * Part: [1920, 1984) MiB. It lies in vgd's claim, which the user has stopped and disabled (11-01), ends under 2^31 because a wl_shm pool's size is an int32 and the pictures are shown straight out of this file, and ends below the card's
 * swap at 2 GiB (/dev/loop0 on /dev/phiblk1, offset 2147483648, read from the card). scripts/mvexflow.sh refuses to start while another card daemon runs. */
#ifndef MF_PROTO_H
#define MF_PROTO_H

#include <stddef.h>
#include <stdint.h>

#include "vt_util.h"

#define MF_PART_OFFSET (1920ull << 20)
#define MF_PART_BYTES (64ull << 20)
#define MF_MAGIC 0x314f4c4658455646ull /* "FVEXFLO1" */

#define MF_SLOTS 6
#define MF_SLOT_BYTES (4608ull << 10) /* holds plane_bytes(1366, 768) = 4,469,760 B */
#define MF_IN_OFFSET (1ull << 20)
#define MF_OUT_OFFSET (MF_IN_OFFSET + MF_SLOTS * MF_SLOT_BYTES)
_Static_assert(MF_OUT_OFFSET + MF_SLOTS * MF_SLOT_BYTES <= MF_PART_BYTES, "the slots fit the part");

#define MF_RING 16 /* frames in flight at most, far above what can be */

enum mf_flags { MF_NO_MID = 1 };
enum mf_times { MF_T_DMA_IN, MF_T_LAND, MF_T_PYR, MF_T_HIST, MF_T_SEARCH, MF_T_FILTER, MF_T_UP, MF_T_MID, MF_T_BLEND, MF_T_DMA_OUT, MF_T_TOTAL, MF_T_COUNT };

struct mf_config {
    uint64_t format;
    uint32_t w, h;
    uint32_t cores, tpc; /* 0 cores: the 60 x 2 of the order of 11-04: 59 usable cores and the free threads of the poller core */
    uint32_t form;       /* enum flow_form */
    uint32_t pad[9];
};

/* Host to card: a frame is ready. */
struct mf_msg {
    uint64_t seq;
    uint32_t in_slot, out_slot;
    uint32_t flags;
    uint32_t pad0;
    uint64_t pad1[5];
};

/* Card to host: done with that frame. */
struct mf_gen {
    uint64_t seq;
    uint32_t out_slot;
    int32_t made;    /* 1: the mid frame between the last frame and this one is in out_slot; 0: scene change; -1: none asked */
    uint32_t moved;  /* blocks with a nonzero mid vector */
    uint32_t us[MF_T_COUNT];
};
_Static_assert(sizeof(struct mf_gen) == 64, "whole line for the DMA");
_Static_assert(sizeof(struct mf_msg) == 64, "whole line");

struct mf_ctl {
    uint64_t magic;
    volatile uint64_t session_seq;
    volatile uint64_t session_end;
    volatile uint64_t quit;
    struct mf_config config;
    /* The card's words, direct uncached stores. */
    volatile uint64_t card_session __attribute__((aligned(64))); /* seq while serving; 0 idle; -seq refused */
    volatile uint64_t card_format;
    volatile uint32_t card_threads, card_cores, card_mhz, card_pad;
    /* Host to card. */
    volatile uint64_t frames_pub __attribute__((aligned(64)));
    struct mf_msg msg[MF_RING];
    /* Card to host, by DMA. */
    volatile uint64_t gen_pub __attribute__((aligned(64)));
    uint64_t pad_gen[7];
    struct mf_gen gen[MF_RING];
};
_Static_assert(sizeof(struct mf_ctl) <= MF_IN_OFFSET, "the control area");

static inline uint64_t mf_format_hash(void)
{
    const uint32_t t[] = {(uint32_t)sizeof(struct mf_ctl), (uint32_t)offsetof(struct mf_ctl, frames_pub), (uint32_t)offsetof(struct mf_ctl, gen_pub), (uint32_t)sizeof(struct mf_config), MF_RING, MF_SLOTS, (uint32_t)MF_SLOT_BYTES, MF_T_COUNT};
    return vt_fnv(VT_FNV_INIT, t, sizeof t);
}

/* Offset in the part of a slot's whole frame, padding included. */
static inline uint64_t mf_in_offset(uint32_t slot) { return MF_IN_OFFSET + slot * MF_SLOT_BYTES; }
static inline uint64_t mf_out_offset(uint32_t slot) { return MF_OUT_OFFSET + slot * MF_SLOT_BYTES; }

#endif

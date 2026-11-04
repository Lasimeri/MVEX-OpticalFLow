/* amdfi.h: how AMD's frame interpolation keeps 8 x 8 motion from showing as blocks, as MVEX: the vector field of FSR 3's optical-flow path.
 *   computeOpticalFlowVectorField   each block's vector, smoothed over the 3 x 3 around it, by weights that favour vectors agreeing with their mean
 *   computeOpticalFlowFieldMvs      laid half its motion away, on the four field places around where the thing is at the middle instant, the highest priority winning: order does not matter
 *   SampleOpticalFlowMotionVectorField   each pixel's own vector, bilinear from the four nearest places: it changes pixel by pixel, so no block edge can exist
 *   SampleTextureBilinear           each frame fetched bilinearly at plus and minus that vector; their mean, or the one frame that has the place
 * Not ported: the game's motion vectors, depth, disocclusion, inpainting: a screen cast gives none of the first three.
 *
 * NOTICE: derives from the FidelityFX SDK shaders ffx_frameinterpolation.h, ffx_frameinterpolation_common.h, ffx_frameinterpolation_optical_flow_vector_field.h, Copyright (C) Advanced Micro Devices, Inc., MIT licence. Full text: LICENSES/FidelityFX-MIT.txt. */
#ifndef MF_AMDFI_H
#define MF_AMDFI_H

#include <stddef.h>
#include <stdint.h>

#include "bgra.h"

struct amdfi {
    int w, h;       /* the field: as many places as the optical flow has blocks */
    uint32_t *x, *y; /* packed as AMD packs them, priority above a vector: half the motion, in sixteenths of a pixel, plus 32768. 0: nothing landed */
};

static inline size_t amdfi_bytes(int w, int h) { return 2u * (size_t)((w + 7) / 8) * (size_t)((h + 7) / 8) * sizeof(uint32_t) + 128u; }
void amdfi_init(struct amdfi *m, void *mem, int w, int h);
void amdfi_clear(const struct amdfi *m);
/* Blocks i0 .. i1 of the field: smoothed and laid down, in parts on any threads: the highest wins whatever the order. */
void amdfi_field_vx(const struct amdfi *m, const struct fg_field *flow, const struct vt_plane *ya, const struct vt_plane *yb, int pic_w, int pic_h, int i0, int i1);
/* AMD's 3 x 3 smoothing alone, in parts on any threads: a field into another, to whole pixels. */
void amdfi_smooth_vx(const struct fg_field *in, const struct fg_field *out, int i0, int i1);
/* The picture, 16 x 16 tiles numbered row by row. */
void amdfi_picture_vx(const struct amdfi *m, const struct bgra *a, const struct bgra *b, const struct bgra *out, int t0, int t1);

#endif

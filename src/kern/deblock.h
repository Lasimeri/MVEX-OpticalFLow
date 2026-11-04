/* deblock.h: the last stage: a generated picture's seams, where neighbouring blocks took different vectors. AMD's SDK has no such pass to port (FidelityFX: none; AMF: an on/off for its hardware encoder), so this is the published one: the normal luma filter of
 * HEVC's in-loop deblocking, ITU-T H.265 8.7.2.5.7, on each of the three colour channels, as MVEX, decided line by line, on the 8 x 8 grid of the vectors. A boundary is filtered only where its two blocks' vectors differ by a pixel or more, H.265's own boundary strength 1, so a
 * real frame, which has no such boundaries, comes through untouched; and only where the picture is smooth on both sides and the step across is small, so the scene's own edges stay.
 *
 * Vertical edges over all rows first, then horizontal, as H.265 has them. In place: no sample one edge writes is read by another of its own direction. */
#ifndef MF_DEBLOCK_H
#define MF_DEBLOCK_H

#include "bgra.h"

/* Rows of 8 pixels numbered from the top: each thread takes a band at a time, in any order. tc: the most a sample may change, grey levels. beta: the most the two sides may curve, added, and stay smooth. */
void deblock_v_vx(const struct fg_field *mid, const struct bgra *pic, float tc, float beta, int band0, int band1);
void deblock_h_vx(const struct fg_field *mid, const struct bgra *pic, float tc, float beta, int band0, int band1);
static inline int deblock_bands(const struct bgra *pic) { return (pic->h + 7) / 8; }

#endif

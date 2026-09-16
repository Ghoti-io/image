/**
 * @file
 *
 * Chroma upsampling and YCbCr->RGB conversion for JPEG decode.  Used by
 * jpeg_entropy.c when assembling component buffers into a raster.  No IDCT or
 * bitstream.
 *
 * Everything here is written against jpeg_plane_t rather than a concrete
 * sample type, so that a 12-bit frame (T.81 Table B.2 allows P=8 and P=12) goes
 * through the same filters as an 8-bit one.  They used to be separate: the
 * 8-bit path had these filters and the 12-bit path had a nearest-neighbour
 * copy, so GIMG_JPEG_CHROMA_UPSAMPLE_FANCY did nothing at all at 12 bits.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <stddef.h>
#include <stdint.h>

#include <ghoti.io/image/macros.h>
#include "jpeg_internal.h"

int jpeg_plane_at(const jpeg_plane_t * p, uint32_t x, uint32_t y) {
  size_t i = (size_t)y * p->stride + (size_t)x;
  return p->wide ? (int)((const uint16_t *)p->data)[i]
                 : (int)((const unsigned char *)p->data)[i];
}

/** Fancy chroma upsampling (triangle filter, 2h2v). ISO/IEC 10918-1 does not
 * mandate a specific upsampling method. Call when the plane is half size in
 * both directions, i.e. cw == ceil(width/2) and ch == ceil(height/2); odd
 * output sizes are fine, the edge cases below clamp. */
int jpeg_chroma_sample_fancy_2h2v(
    const jpeg_plane_t * p, uint32_t cw, uint32_t ch, uint32_t x, uint32_t y) {
  uint32_t ri = y >> 1;
  uint32_t ri_other =
      (y & 1u) ? (ri + 1 < ch ? ri + 1 : ri) : (ri ? ri - 1 : 0);
  if (x == 0) {
    int cur = jpeg_plane_at(p, 0, ri);
    int other = jpeg_plane_at(p, 0, ri_other);
    return ((cur * 3 + other) * 4 + 8) >> 4;
  }
  if (x == 2u * cw - 1u) {
    int cur = jpeg_plane_at(p, cw - 1u, ri);
    int other = jpeg_plane_at(p, cw - 1u, ri_other);
    return ((cur * 3 + other) * 4 + 7) >> 4;
  }
  if (x & 1u) {
    uint32_t ci = (x - 1) >> 1;
    uint32_t ci1 = (ci + 1 < cw) ? ci + 1 : ci;
    int tl = jpeg_plane_at(p, ci, ri);
    int tr = jpeg_plane_at(p, ci1, ri);
    int bl = jpeg_plane_at(p, ci, ri_other);
    int br = jpeg_plane_at(p, ci1, ri_other);
    int thiscolsum = tl * 3 + bl;
    int nextcolsum = tr * 3 + br;
    return (thiscolsum * 3 + nextcolsum + 7) >> 4;
  }
  uint32_t ci = x >> 1;
  uint32_t cim1 = ci - 1;
  int tl = jpeg_plane_at(p, ci, ri);
  int tr = jpeg_plane_at(p, cim1, ri);
  int bl = jpeg_plane_at(p, ci, ri_other);
  int br = jpeg_plane_at(p, cim1, ri_other);
  int thiscolsum = tl * 3 + bl;
  int lastcolsum = tr * 3 + br;
  return (thiscolsum * 3 + lastcolsum + 8) >> 4;
}

/**
 * Fancy chroma upsampling for 4:2:2 (h2v1): triangle filter horizontally, no
 * vertical interpolation because the plane is already full height.
 *
 * This matches libjpeg's h2v1_fancy_upsample.  Without it a 4:2:2 file was
 * always box-filtered no matter what the caller asked for, because only the
 * 2h2v filter existed - so GIMG_JPEG_CHROMA_UPSAMPLE_FANCY was silently a
 * no-op for half the subsampled files in the world.
 */
int jpeg_chroma_sample_fancy_h2v1(
    const jpeg_plane_t * p, uint32_t cw, uint32_t ch, uint32_t x, uint32_t y) {
  (void)ch;
  if (x == 0) {
    return jpeg_plane_at(p, 0, y);
  }
  if (x >= 2u * cw - 1u) {
    return jpeg_plane_at(p, cw - 1u, y);
  }
  uint32_t ci = x >> 1;
  if (x & 1u) {
    uint32_t ci1 = (ci + 1u < cw) ? ci + 1u : ci;
    return (jpeg_plane_at(p, ci, y) * 3 + jpeg_plane_at(p, ci1, y) + 2) >> 2;
  }
  return (jpeg_plane_at(p, ci, y) * 3 + jpeg_plane_at(p, ci - 1u, y) + 1) >> 2;
}

int jpeg_chroma_sample(const jpeg_plane_t * p, uint32_t cw, uint32_t ch,
    uint32_t x, uint32_t y, uint32_t width, uint32_t height, uint8_t h_samp,
    uint8_t v_samp, uint8_t h_max, uint8_t v_max, int fancy) {
  if (fancy && cw > 0u && ch > 0u) {
    // Pick the filter from the component's sampling factors (T.81 B.2.2 Hi and
    // Vi), which is what they are for, rather than from the shape of the
    // decoded plane.  The plane's shape cannot answer this: a 16x1 image
    // subsampled 2x1 and the same image subsampled 2x2 both produce an 8x1
    // chroma plane, and the two want different filters.  Inferring from
    // dimensions picked the wrong one for every single-row 4:2:2 or 4:2:0
    // frame.  libjpeg (jdsample.c, start_pass_upsample) selects exactly this
    // way.
    int h_half = ((int)h_samp * 2 == (int)h_max);
    int v_full = (v_samp == v_max);
    int v_half = ((int)v_samp * 2 == (int)v_max);
    // A triangle filter needs a neighbour on each side to interpolate between.
    // With one or two columns there is no interior, and the filter degenerates
    // into a weighted copy of the same one or two samples - which is not what
    // libjpeg produces there: jdsample.c selects the fancy upsamplers only when
    // downsampled_width > 2 and falls back to the box filter otherwise.  Match
    // that, so a one-pixel-wide image decodes the same way everywhere.
    if (cw > 2u && h_half) {
      if (v_full) {
        return jpeg_chroma_sample_fancy_h2v1(p, cw, ch, x, y);
      }
      if (v_half) {
        return jpeg_chroma_sample_fancy_2h2v(p, cw, ch, x, y);
      }
    }
  }
  uint32_t cx = (cw > 1u && width > 1u) ? (x * cw / width) : 0u;
  uint32_t cy = (ch > 1u && height > 1u) ? (y * ch / height) : 0u;
  return jpeg_plane_at(p, cx, cy);
}

/**
 * YCbCr -> RGB, the conversion JFIF specifies and every decoder implements with
 * the same scaled-integer arithmetic (libjpeg jdcolor.c, SCALEBITS = 16):
 *
 *   R = Y                        + 1.40200 * (Cr - centre)
 *   G = Y - 0.34414 * (Cb - centre) - 0.71414 * (Cr - centre)
 *   B = Y + 1.77200 * (Cb - centre)
 *
 * Only Cb and Cr are centred; Y is an unsigned sample and is used as it stands.
 * @param centre 2^(P-1): 128 at P=8, 2048 at P=12.
 * @param max_val 2^P - 1, the clamp T.81 A.3.1 requires of a reconstructed
 *   sample.
 *
 * The 12-bit path used to do this in floating point, as
 * "yy = Y - centre; r = yy + (int)(1.402 * cr + 0.5)" - which subtracts the
 * centre from Y and never adds it back, so every 12-bit colour sample we ever
 * produced was low by very nearly half the range.  (int)(x + 0.5) also rounds
 * negative values towards zero rather than to nearest.  Nothing caught either,
 * because nothing outside this library had ever decoded a 12-bit file we wrote.
 */
void jpeg_ycbcr_to_rgb(int y, int cb, int cr, int centre, int max_val,
    int * out_r, int * out_g, int * out_b) {
  int cb_x = cb - centre;
  int cr_x = cr - centre;
  // FIX(1.40200) = 91881, FIX(0.34414) = 22554, FIX(0.71414) = 46802,
  // FIX(1.77200) = 116130, and ONE_HALF = 1 << 15 for rounding.  The shift is
  // arithmetic, so it floors, which is what the reference tables do.
  int r = y + (int)((91881L * cr_x + 32768) >> 16);
  int g = y +
      (int)(((int32_t)(-22554) * cb_x + (int32_t)(-46802) * cr_x + 32768) >> 16);
  int b = y + (int)((116130L * cb_x + 32768) >> 16);
  if (r < 0)
    r = 0;
  if (r > max_val)
    r = max_val;
  if (g < 0)
    g = 0;
  if (g > max_val)
    g = max_val;
  if (b < 0)
    b = 0;
  if (b > max_val)
    b = max_val;
  *out_r = r;
  *out_g = g;
  *out_b = b;
}

/**
 * @file
 *
 * Chroma upsampling for JPEG decode (e.g. 2h2v fancy filter). Used by
 * jpeg_entropy.c when assembling component buffers to raster. No IDCT or
 * bitstream.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <stddef.h>
#include <stdint.h>

#include <ghoti.io/image/macros.h>
#include "jpeg_internal.h"

/** Fancy chroma upsampling (triangle filter, 2h2v). ISO/IEC 10918-1 does not
 * mandate a specific upsampling method. Call when the plane is half size in
 * both directions, i.e. cw == ceil(width/2) and ch == ceil(height/2); odd
 * output sizes are fine, the edge cases below clamp. */
int jpeg_chroma_sample_fancy_2h2v(const unsigned char * buf,
    size_t stride, uint32_t cw, uint32_t ch, uint32_t x, uint32_t y) {
  uint32_t ri = y >> 1;
  uint32_t ri_other =
      (y & 1u) ? (ri + 1 < ch ? ri + 1 : ri) : (ri ? ri - 1 : 0);
  if (x == 0) {
    int cur = (int)buf[ri * stride + 0];
    int other = (int)buf[ri_other * stride + 0];
    return ((cur * 3 + other) * 4 + 8) >> 4;
  }
  if (x == 2u * cw - 1u) {
    int cur = (int)buf[ri * stride + (cw - 1)];
    int other = (int)buf[ri_other * stride + (cw - 1)];
    return ((cur * 3 + other) * 4 + 7) >> 4;
  }
  if (x & 1u) {
    uint32_t ci = (x - 1) >> 1;
    uint32_t ci1 = (ci + 1 < cw) ? ci + 1 : ci;
    int tl = (int)buf[ri * stride + ci];
    int tr = (int)buf[ri * stride + ci1];
    int bl = (int)buf[ri_other * stride + ci];
    int br = (int)buf[ri_other * stride + ci1];
    int thiscolsum = tl * 3 + bl;
    int nextcolsum = tr * 3 + br;
    return (thiscolsum * 3 + nextcolsum + 7) >> 4;
  }
  uint32_t ci = x >> 1;
  uint32_t cim1 = ci - 1;
  int tl = (int)buf[ri * stride + ci];
  int tr = (int)buf[ri * stride + cim1];
  int bl = (int)buf[ri_other * stride + ci];
  int br = (int)buf[ri_other * stride + cim1];
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
int jpeg_chroma_sample_fancy_h2v1(const unsigned char * buf, size_t stride,
    uint32_t cw, uint32_t ch, uint32_t x, uint32_t y) {
  (void)ch;
  const unsigned char * row = buf + (size_t)y * stride;
  if (x == 0) {
    return (int)row[0];
  }
  if (x >= 2u * cw - 1u) {
    return (int)row[cw - 1u];
  }
  uint32_t ci = x >> 1;
  if (x & 1u) {
    uint32_t ci1 = (ci + 1u < cw) ? ci + 1u : ci;
    return ((int)row[ci] * 3 + (int)row[ci1] + 2) >> 2;
  }
  return ((int)row[ci] * 3 + (int)row[ci - 1u] + 1) >> 2;
}

int jpeg_chroma_sample(const unsigned char * buf, size_t stride, uint32_t cw,
    uint32_t ch, uint32_t x, uint32_t y, uint32_t width, uint32_t height,
    int fancy) {
  if (fancy && cw > 0u && ch > 0u) {
    // The plane is half width when cw == ceil(width/2).  Testing width == cw*2
    // instead, as this used to, is false for every odd width - 129 against a
    // 65-wide plane - so odd-sized images quietly lost the filter they asked
    // for.  The filters clamp at the edges and handle the odd case fine.
    int half_w = (cw == (width + 1u) / 2u);
    int half_h = (ch == (height + 1u) / 2u);
    // A triangle filter needs a neighbour on each side to interpolate between.
    // With one or two columns there is no interior, and the filter degenerates
    // into a weighted copy of the same one or two samples - which is not what
    // libjpeg produces there: jdsample.c selects the fancy upsamplers only when
    // downsampled_width > 2 and falls back to the box filter otherwise.  Match
    // that, so a one-pixel-wide image decodes the same way everywhere.
    if (cw > 2u) {
      if (half_w && half_h) {
        return jpeg_chroma_sample_fancy_2h2v(buf, stride, cw, ch, x, y);
      }
      if (half_w && ch == height) {
        return jpeg_chroma_sample_fancy_h2v1(buf, stride, cw, ch, x, y);
      }
    }
  }
  uint32_t cx = (cw > 1u && width > 1u) ? (x * cw / width) : 0u;
  uint32_t cy = (ch > 1u && height > 1u) ? (y * ch / height) : 0u;
  return (int)buf[(size_t)cy * stride + cx];
}

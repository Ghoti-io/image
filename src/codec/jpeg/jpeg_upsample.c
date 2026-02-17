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

#include "jpeg_internal.h"

/** Fancy chroma upsampling (triangle filter, 2h2v). ISO/IEC 10918-1 does not
 * mandate a specific upsampling method. Call only when width==cw*2 and
 * height==ch*2. */
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

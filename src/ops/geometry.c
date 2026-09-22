/*
 * SPDX-License-Identifier: LGPL-3.0-only
 *
 * Copyright (C) 2026 Corey Pennycuff
 *
 * This file is part of Ghoti.io Image.
 *
 * Ghoti.io Image is free software: you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License version 3 as
 * published by the Free Software Foundation.
 *
 * Ghoti.io Image is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public
 * License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/**
 * @file
 *
 * Geometry that moves samples without interpreting them: cropping.
 *
 * Everything here is byte arithmetic on whole pixels, so it works for any
 * format whose pixel occupies a whole number of bytes - which is every format
 * a raster in this library can hold.  That is a wider reach than the
 * resampler in resample.c has, and deliberately: a crop never has to know
 * what a sample means, so it never has to refuse a channel model it has no
 * arithmetic for.
 *
 * Rotation and mirroring are not here.  gimg_ops_apply_orientation() already
 * implements all eight of CIPA DC-008 Table 6 - both axis mirrors, both
 * diagonal mirrors and all three quarter turns - by the same kind of index
 * remap, and a second implementation of the same six transforms would be two
 * copies of one loop waiting to drift apart.
 */

#include <ghoti.io/image/macros.h>
#include <ghoti.io/image/meta.h>
#include <ghoti.io/image/ops.h>
#include <ghoti.io/image/raster.h>
#include <string.h>

#include "../core/safe_math_internal.h"
#include "../raster/raster_internal.h"
#include "ops_internal.h"

GIMG_API GIMG_Result gimg_ops_crop(const GIMG_Raster * src, uint32_t x,
    uint32_t y, uint32_t width, uint32_t height, GIMG_Raster ** out_raster) {
  if (!src || !out_raster) {
    return GIMG_ERR_INTERNAL;
  }
  *out_raster = NULL;

  const uint32_t src_w = gimg_raster_width(src);
  const uint32_t src_h = gimg_raster_height(src);

  // An empty rectangle is refused rather than yielding an empty raster: every
  // other part of this library takes a raster's dimensions to be above zero,
  // and gimg_raster_create rejects a zero one anyway.
  if (width == 0u || height == 0u) {
    return GIMG_ERR_INTERNAL;
  }
  // Written as a subtraction so that neither sum can wrap: x + width overflows
  // uint32_t for a large enough x, and the obvious bounds test would then pass
  // for a rectangle that starts past the end of the image.
  if (width > src_w || x > src_w - width) {
    return GIMG_ERR_INTERNAL;
  }
  if (height > src_h || y > src_h - height) {
    return GIMG_ERR_INTERNAL;
  }

  const GIMG_Pixel_Format * fmt = gimg_raster_format(src);
  const size_t bpp = gimg_raster_bytes_per_pixel(fmt);
  if (bpp == 0u) {
    return GIMG_ERR_UNSUPPORTED;
  }

  const void * src_pixels = gimg_raster_pixels_const(src);
  if (!src_pixels) {
    return GIMG_ERR_INTERNAL;
  }

  size_t row_bytes = 0u;
  if (!gcu_safe_mul_size((size_t)width, bpp, &row_bytes)) {
    return GIMG_ERR_LIMIT;
  }

  GIMG_Result r = gimg_raster_create_with_allocator(gimg_raster_allocator(src),
      width, height, fmt, GIMG_RASTER_OWNED, NULL, 0, out_raster);
  if (r != GIMG_OK) {
    return r;
  }

  const size_t src_stride = gimg_raster_stride_bytes(src);
  const size_t dst_stride = gimg_raster_stride_bytes(*out_raster);
  const unsigned char * sp = (const unsigned char *)src_pixels +
      ((size_t)y * src_stride) + ((size_t)x * bpp);
  unsigned char * dp = (unsigned char *)gimg_raster_pixels(*out_raster);

  for (uint32_t row = 0u; row < height; row++) {
    memcpy(dp, sp, row_bytes);
    sp += src_stride;
    dp += dst_stride;
  }

  r = gimg_ops_carry_color(src, *out_raster);
  if (r != GIMG_OK) {
    gimg_raster_destroy(*out_raster);
    *out_raster = NULL;
    return r;
  }
  return GIMG_OK;
}

/**
 * Whether two formats describe the same bytes, so that samples can be moved
 * or blended between them without reinterpreting anything.
 */
static bool gimg_geometry_same_format(
    const GIMG_Pixel_Format * a, const GIMG_Pixel_Format * b) {
  if (a->channel_model != b->channel_model ||
      a->channel_type != b->channel_type || a->layout != b->layout ||
      a->channel_count != b->channel_count) {
    return false;
  }
  for (uint8_t i = 0; i < a->channel_count && i < 8u; i++) {
    if (a->bits_per_channel[i] != b->bits_per_channel[i]) {
      return false;
    }
  }
  return true;
}

/**
 * Porter-Duff "over" on straight alpha, at one pixel.
 *
 * The composite alpha is the usual a + b(1 - a).  The colour is the weighted
 * mean of the two contributions divided back out by it, which is what keeps
 * the result straight rather than premultiplied - the arithmetic is done
 * premultiplied because that is the only form in which the two contributions
 * add, and undone at the end because that is the form the raster holds.
 *
 * Where the result is wholly transparent there is no colour to report and the
 * division has no answer; the channels are set to zero rather than left
 * holding whatever the destination had, so that two runs over different
 * destinations give the same bytes.
 */
static void gimg_composite_over_pixel(unsigned char * dst_pixel,
    const unsigned char * src_pixel, bool wide, uint32_t max) {
  const uint32_t src_a = wide ? ((const uint16_t *)src_pixel)[3]
                              : src_pixel[3];
  const uint32_t dst_a = wide ? ((const uint16_t *)dst_pixel)[3]
                              : dst_pixel[3];
  if (src_a == max) {
    // Fully opaque source: the destination contributes nothing at all, and
    // going through the general arithmetic would only risk losing a count.
    memcpy(dst_pixel, src_pixel, (size_t)4u * (wide ? 2u : 1u));
    return;
  }
  if (src_a == 0u && dst_a != 0u) {
    // Nothing to add, and the general arithmetic would return the
    // destination unchanged anyway: out_a comes to dst_a and each channel to
    // its own value.  The dst_a == 0 case deliberately does *not* take this
    // path - there the result is wholly transparent and its colour has to be
    // settled to zero, or two invisible pixels that started differently stay
    // different and a byte comparison of two composites disagrees about
    // pixels nobody can see.
    return;
  }
  const uint32_t keep = max - src_a; // the fraction of dst that survives
  // out_a = src_a + dst_a * keep / max, rounded to nearest.
  const uint32_t out_a = src_a + (dst_a * keep + max / 2u) / max;
  for (uint8_t c = 0; c < 3u; c++) {
    const uint32_t sc =
        wide ? ((const uint16_t *)src_pixel)[c] : src_pixel[c];
    const uint32_t dc =
        wide ? ((const uint16_t *)dst_pixel)[c] : dst_pixel[c];
    // Premultiplied contributions, summed, then divided back out by out_a.
    const uint64_t num = (uint64_t)sc * src_a * max +
        (uint64_t)dc * dst_a * keep;
    uint32_t value = 0u;
    if (out_a != 0u) {
      const uint64_t den = (uint64_t)out_a * max;
      value = (uint32_t)((num + den / 2u) / den);
      if (value > max) {
        value = max;
      }
    }
    if (wide) {
      ((uint16_t *)dst_pixel)[c] = (uint16_t)value;
    }
    else {
      dst_pixel[c] = (unsigned char)value;
    }
  }
  if (wide) {
    ((uint16_t *)dst_pixel)[3] = (uint16_t)out_a;
  }
  else {
    dst_pixel[3] = (unsigned char)out_a;
  }
}

GIMG_API GIMG_Result gimg_ops_composite(GIMG_Raster * dst,
    const GIMG_Raster * src, int32_t x, int32_t y, GIMG_Composite_Op op) {
  if (!dst || !src) {
    return GIMG_ERR_INTERNAL;
  }
  if (op != GIMG_COMPOSITE_SOURCE && op != GIMG_COMPOSITE_OVER) {
    return GIMG_ERR_UNSUPPORTED;
  }
  const GIMG_Pixel_Format * dst_fmt = gimg_raster_format(dst);
  const GIMG_Pixel_Format * src_fmt = gimg_raster_format(src);
  if (!gimg_geometry_same_format(dst_fmt, src_fmt)) {
    // Converting on the caller's behalf would be changing an image's colour
    // without being asked, which nothing else in this library does either.
    return GIMG_ERR_UNSUPPORTED;
  }
  const size_t bpp = gimg_raster_bytes_per_pixel(dst_fmt);
  if (bpp == 0u) {
    return GIMG_ERR_UNSUPPORTED;
  }

  const bool has_alpha =
      (dst_fmt->channel_model == GIMG_CHANNEL_RGBA &&
          dst_fmt->channel_count == 4u);
  const uint8_t bits = dst_fmt->bits_per_channel[0];
  if (op == GIMG_COMPOSITE_OVER) {
    if (!has_alpha) {
      // "Over" is a statement about what covers what, and a format with no
      // alpha has not said. Nothing sensible distinguishes it from a plain
      // copy, so asking for it is a mistake worth reporting.
      return GIMG_ERR_UNSUPPORTED;
    }
    if (bits != 8u && bits != 16u) {
      return GIMG_ERR_UNSUPPORTED;
    }
  }

  const int64_t dst_w = (int64_t)gimg_raster_width(dst);
  const int64_t dst_h = (int64_t)gimg_raster_height(dst);
  const int64_t src_w = (int64_t)gimg_raster_width(src);
  const int64_t src_h = (int64_t)gimg_raster_height(src);

  // Clip in 64-bit, so that an offset near the end of the int32 range cannot
  // wrap its way back inside the destination.
  int64_t x0 = (int64_t)x;
  int64_t y0 = (int64_t)y;
  int64_t x1 = x0 + src_w;
  int64_t y1 = y0 + src_h;
  if (x0 < 0) {
    x0 = 0;
  }
  if (y0 < 0) {
    y0 = 0;
  }
  if (x1 > dst_w) {
    x1 = dst_w;
  }
  if (y1 > dst_h) {
    y1 = dst_h;
  }
  if (x1 <= x0 || y1 <= y0) {
    // Entirely outside the destination. That is a placement, not a mistake -
    // a frame scrolled off the canvas is an ordinary thing for an animation
    // to do - so nothing happens and nothing is reported.
    return GIMG_OK;
  }

  const bool wide = (bits == 16u);
  const uint32_t max = (bits >= 32u) ? UINT32_MAX
                                     : ((UINT32_C(1) << bits) - 1u);
  unsigned char * dp = (unsigned char *)gimg_raster_pixels(dst);
  const unsigned char * sp =
      (const unsigned char *)gimg_raster_pixels_const(src);
  if (!dp || !sp) {
    return GIMG_ERR_INTERNAL;
  }
  const size_t dst_stride = gimg_raster_stride_bytes(dst);
  const size_t src_stride = gimg_raster_stride_bytes(src);
  const size_t run = (size_t)(x1 - x0) * bpp;

  for (int64_t row = y0; row < y1; row++) {
    unsigned char * drow = dp + ((size_t)row * dst_stride) +
        ((size_t)x0 * bpp);
    const unsigned char * srow = sp +
        ((size_t)(row - (int64_t)y) * src_stride) +
        ((size_t)(x0 - (int64_t)x) * bpp);
    if (op == GIMG_COMPOSITE_SOURCE) {
      memcpy(drow, srow, run);
      continue;
    }
    for (int64_t col = x0; col < x1; col++) {
      gimg_composite_over_pixel(drow, srow, wide, max);
      drow += bpp;
      srow += bpp;
    }
  }
  return GIMG_OK;
}

//
// The mirrors and quarter turns, under the names a caller reaches for.
//
// Every one of these is a CIPA DC-008 Table 6 orientation, and
// gimg_ops_apply_orientation already implements all eight. These exist because
// GIMG_ORIENTATION_TRANSVERSE is not what anybody searches for when they want
// to turn a picture, and a caller who cannot find the operation writes their
// own loop. They forward rather than reimplement: two copies of one index
// remap are two things to keep in step.
//

GIMG_API GIMG_Result gimg_ops_flip_horizontal(GIMG_Raster * raster) {
  return gimg_ops_apply_orientation(raster, GIMG_ORIENTATION_FLIP_H);
}

GIMG_API GIMG_Result gimg_ops_flip_vertical(GIMG_Raster * raster) {
  return gimg_ops_apply_orientation(raster, GIMG_ORIENTATION_FLIP_V);
}

GIMG_API GIMG_Result gimg_ops_rotate_90_cw(GIMG_Raster * raster) {
  return gimg_ops_apply_orientation(raster, GIMG_ORIENTATION_ROTATE_90_CW);
}

GIMG_API GIMG_Result gimg_ops_rotate_90_ccw(GIMG_Raster * raster) {
  return gimg_ops_apply_orientation(raster, GIMG_ORIENTATION_ROTATE_90_CCW);
}

GIMG_API GIMG_Result gimg_ops_rotate_180(GIMG_Raster * raster) {
  return gimg_ops_apply_orientation(raster, GIMG_ORIENTATION_ROTATE_180);
}

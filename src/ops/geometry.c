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

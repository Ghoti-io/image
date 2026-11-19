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
 * ICO AND-mask → alpha, including the identically-zero 32-bpp alpha fallback.
 */

#include <ghoti.io/image/macros.h>
#include <string.h>

#include "ico_internal.h"

GIMG_Result gimg_ico_apply_and_mask(GIMG_Raster * raster,
    const unsigned char * mask, size_t mask_size, int force, int top_down) {
  if (!raster || (!mask && force)) {
    return GIMG_ERR_INTERNAL;
  }
  const GIMG_Pixel_Format * fmt = gimg_raster_format(raster);
  if (!fmt || fmt->channel_model != GIMG_CHANNEL_RGBA ||
      fmt->bits_per_channel[0] != 8u) {
    return GIMG_ERR_UNSUPPORTED;
  }

  uint32_t width = gimg_raster_width(raster);
  uint32_t height = gimg_raster_height(raster);
  // AND mask rows are 1 bit per pixel, padded to 32-bit boundaries, in the
  // same row order as the XOR bitmap (bottom-up unless the DIB is top-down).
  size_t row_bytes = ((size_t)width + 31u) / 32u * 4u;
  size_t needed = row_bytes * (size_t)height;
  int mask_usable = mask && needed <= mask_size;
  if (force && !mask_usable) {
    return GIMG_ERR_CORRUPT;
  }

  uint8_t * pixels = (uint8_t *)gimg_raster_pixels(raster);
  size_t stride = gimg_raster_stride_bytes(raster);

  if (!force) {
    // Alpha-fallback path: only apply when every alpha byte is zero.
    int any_alpha = 0;
    for (uint32_t y = 0; y < height && !any_alpha; y++) {
      const uint8_t * row = pixels + (size_t)y * stride;
      for (uint32_t x = 0; x < width; x++) {
        if (row[x * 4u + 3u] != 0u) {
          any_alpha = 1;
          break;
        }
      }
    }
    if (any_alpha) {
      return GIMG_OK;
    }
    if (!mask_usable) {
      // An identically-zero alpha channel with no complete mask behind it
      // states nothing: the writer left both unset, so the image is opaque.
      for (uint32_t y = 0; y < height; y++) {
        uint8_t * row = pixels + (size_t)y * stride;
        for (uint32_t x = 0; x < width; x++) {
          row[x * 4u + 3u] = 255u;
        }
      }
      return GIMG_OK;
    }
  }

  for (uint32_t y = 0; y < height; y++) {
    // The raster is top-down; the mask is bottom-up unless the DIB is not.
    size_t src_row = top_down ? y : (size_t)(height - 1u - y);
    const unsigned char * mrow = mask + (src_row * row_bytes);
    uint8_t * row = pixels + (size_t)y * stride;
    for (uint32_t x = 0; x < width; x++) {
      unsigned bit = (mrow[x / 8u] >> (7u - (x % 8u))) & 1u;
      // Set bit = transparent in the AND mask.
      row[x * 4u + 3u] = bit ? 0u : 255u;
    }
  }
  return GIMG_OK;
}

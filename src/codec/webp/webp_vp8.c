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
 * VP8 keyframe decode is not implemented. The previous decoder was libwebp's
 * own source, compiled in from a tree under this directory. That tree is not
 * part of this library. The oracle for a lossy file is dwebp, in the pinned
 * container, and it is not linked here.
 */

#include <ghoti.io/image/macros.h>
#include <stdint.h>

#include "webp_internal.h"

int gimg_webp_peek_vp8_dims(const unsigned char * data, size_t size,
    uint32_t * out_w, uint32_t * out_h) {
  if (!data || !out_w || !out_h || size < 10u) {
    return 0;
  }
  /* Key frame, start-code 0x9d012a, then 14-bit width and height. */
  if ((data[0] & 1u) != 0u) {
    return 0;
  }
  if (data[3] != 0x9du || data[4] != 0x01u || data[5] != 0x2au) {
    return 0;
  }
  uint16_t raw_w =
      (uint16_t)((uint32_t)data[6] | ((uint32_t)data[7] << 8));
  uint16_t raw_h =
      (uint16_t)((uint32_t)data[8] | ((uint32_t)data[9] << 8));
  *out_w = (uint32_t)(raw_w & 0x3fffu);
  *out_h = (uint32_t)(raw_h & 0x3fffu);
  return (*out_w > 0u && *out_h > 0u) ? 1 : 0;
}

int gimg_webp_peek_vp8l_dims(const unsigned char * data, size_t size,
    uint32_t * out_w, uint32_t * out_h, int * out_alpha) {
  if (!data || !out_w || !out_h || size < 5u) {
    return 0;
  }
  if (data[0] != 0x2fu) {
    return 0;
  }
  uint32_t bits = (uint32_t)data[1] | ((uint32_t)data[2] << 8) |
      ((uint32_t)data[3] << 16) | ((uint32_t)data[4] << 24);
  *out_w = (bits & 0x3fffu) + 1u;
  *out_h = ((bits >> 14) & 0x3fffu) + 1u;
  if (out_alpha) {
    *out_alpha = (int)((bits >> 28) & 1u);
  }
  uint32_t version = (bits >> 29) & 7u;
  return (version == 0u && *out_w > 0u && *out_h > 0u) ? 1 : 0;
}

GIMG_Result gimg_webp_vp8_decode(const unsigned char * data, size_t size,
    const GIMG_Allocator * alloc, GIMG_Raster ** out_raster) {
  (void)data;
  (void)size;
  (void)alloc;
  if (!out_raster) {
    return GIMG_ERR_INTERNAL;
  }
  *out_raster = NULL;
  return GIMG_ERR_UNSUPPORTED;
}

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
 * WebP ALPH chunk decode (Phase C). Matches libwebp 1.5.0 alpha_dec.c /
 * filters.c unfilters. Level-reduction dithering is not applied: default
 * dwebp leaves alpha_dithering at 0, so PAM alpha is the unfiltered plane.
 */

#include <ghoti.io/image/macros.h>
#include <string.h>

#include "../../core/alloc_internal.h"
#include "webp_internal.h"

enum {
  ALPHA_NO_COMPRESSION = 0,
  ALPHA_LOSSLESS_COMPRESSION = 1,
  ALPHA_PREPROCESSED_LEVELS = 1,
  WEBP_FILTER_NONE = 0,
  WEBP_FILTER_HORIZONTAL = 1,
  WEBP_FILTER_VERTICAL = 2,
  WEBP_FILTER_GRADIENT = 3,
  WEBP_FILTER_LAST = 4
};

static int gradient_predictor(uint8_t a, uint8_t b, uint8_t c) {
  const int g = (int)a + (int)b - (int)c;
  return ((g & ~0xff) == 0) ? g : (g < 0) ? 0 : 255;
}

static void unfilter_none(const uint8_t * prev, const uint8_t * in, uint8_t * out,
    int width) {
  (void)prev;
  if (out != in) {
    memcpy(out, in, (size_t)width);
  }
}

static void unfilter_horizontal(const uint8_t * prev, const uint8_t * in,
    uint8_t * out, int width) {
  uint8_t pred = (prev == NULL) ? 0u : prev[0];
  for (int i = 0; i < width; ++i) {
    out[i] = (uint8_t)(pred + in[i]);
    pred = out[i];
  }
}

static void unfilter_vertical(const uint8_t * prev, const uint8_t * in,
    uint8_t * out, int width) {
  if (prev == NULL) {
    unfilter_horizontal(NULL, in, out, width);
    return;
  }
  for (int i = 0; i < width; ++i) {
    out[i] = (uint8_t)(prev[i] + in[i]);
  }
}

static void unfilter_gradient(const uint8_t * prev, const uint8_t * in,
    uint8_t * out, int width) {
  if (prev == NULL) {
    unfilter_horizontal(NULL, in, out, width);
    return;
  }
  {
    uint8_t top = prev[0];
    uint8_t top_left = top;
    uint8_t left = top;
    for (int i = 0; i < width; ++i) {
      top = prev[i];
      left = (uint8_t)(in[i] + gradient_predictor(left, top, top_left));
      top_left = top;
      out[i] = left;
    }
  }
}

typedef void (*unfilter_fn)(const uint8_t * prev, const uint8_t * in,
    uint8_t * out, int width);

static unfilter_fn unfilter_for(int filter) {
  switch (filter) {
  case WEBP_FILTER_NONE:
    return unfilter_none;
  case WEBP_FILTER_HORIZONTAL:
    return unfilter_horizontal;
  case WEBP_FILTER_VERTICAL:
    return unfilter_vertical;
  case WEBP_FILTER_GRADIENT:
    return unfilter_gradient;
  default:
    return NULL;
  }
}

static void apply_filter_inplace(uint8_t * plane, int width, int height,
    int filter) {
  unfilter_fn fn = unfilter_for(filter);
  const uint8_t * prev = NULL;
  if (!fn || filter == WEBP_FILTER_NONE) {
    return;
  }
  for (int y = 0; y < height; ++y) {
    uint8_t * row = plane + (size_t)y * (size_t)width;
    fn(prev, row, row, width);
    prev = row;
  }
}

GIMG_Result gimg_webp_alpha_decode(const unsigned char * data, size_t size,
    uint32_t width, uint32_t height, const GIMG_Allocator * alloc,
    uint8_t ** out_alpha) {
  if (!data || !out_alpha || width == 0u || height == 0u) {
    return GIMG_ERR_INTERNAL;
  }
  *out_alpha = NULL;
  if (size < 1u) {
    return GIMG_ERR_CORRUPT;
  }

  alloc = gimg_alloc_or_default(alloc);
  const int method = (int)(data[0] & 0x03u);
  const int filter = (int)((data[0] >> 2) & 0x03u);
  const int pre = (int)((data[0] >> 4) & 0x03u);
  const int rsv = (int)((data[0] >> 6) & 0x03u);
  if (method < ALPHA_NO_COMPRESSION || method > ALPHA_LOSSLESS_COMPRESSION ||
      filter >= WEBP_FILTER_LAST || pre > ALPHA_PREPROCESSED_LEVELS ||
      rsv != 0) {
    return GIMG_ERR_CORRUPT;
  }
  if (!unfilter_for(filter)) {
    return GIMG_ERR_CORRUPT;
  }

  const unsigned char * body = data + 1;
  const size_t body_size = size - 1u;
  const size_t plane_n = (size_t)width * (size_t)height;
  uint8_t * plane = (uint8_t *)gimg_malloc(alloc, plane_n);
  if (!plane) {
    return GIMG_ERR_OOM;
  }

  if (method == ALPHA_NO_COMPRESSION) {
    if (body_size < plane_n) {
      gimg_free(alloc, plane);
      return GIMG_ERR_CORRUPT;
    }
    {
      const uint8_t * prev = NULL;
      unfilter_fn fn = unfilter_for(filter);
      for (uint32_t y = 0; y < height; ++y) {
        const uint8_t * in = body + (size_t)y * (size_t)width;
        uint8_t * out = plane + (size_t)y * (size_t)width;
        fn(prev, in, out, (int)width);
        prev = out;
      }
    }
  }
  else {
    /* VP8L-compressed: decode green plane, then unfilter. */
    uint8_t * green = NULL;
    GIMG_Result r = gimg_webp_vp8l_decode_alpha(
        body, body_size, width, height, alloc, &green);
    if (r != GIMG_OK) {
      gimg_free(alloc, plane);
      return r;
    }
    memcpy(plane, green, plane_n);
    gimg_free(alloc, green);
    apply_filter_inplace(plane, (int)width, (int)height, filter);
  }

  /* pre_processing == ALPHA_PREPROCESSED_LEVELS enables optional dithered
   * dequantisation in libwebp. Default dwebp leaves dithering at 0, so the
   * plane above already matches PAM alpha. */
  (void)pre;

  *out_alpha = plane;
  return GIMG_OK;
}

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
 * WebP Phase E: ANMF header parse and canvas compositing (dispose / blend)
 * matching libwebp's anim_dump.
 */

#include <ghoti.io/image/macros.h>
#include <ghoti.io/image/raster.h>

#include <string.h>

#include "../../core/alloc_internal.h"
#include "webp_internal.h"

static uint32_t webp_u24(const unsigned char * p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
}

GIMG_Result gimg_webp_parse_anmf_header(const unsigned char * header16,
    gimg_webp_frame_t * frame) {
  if (!header16 || !frame) {
    return GIMG_ERR_INTERNAL;
  }
  memset(frame, 0, sizeof(*frame));
  // On-disk X/Y are half-pixels; multiply by two (libwebp demux).
  frame->x = webp_u24(header16) * 2u;
  frame->y = webp_u24(header16 + 3) * 2u;
  frame->width = webp_u24(header16 + 6) + 1u;
  frame->height = webp_u24(header16 + 9) + 1u;
  frame->duration_ms = webp_u24(header16 + 12);
  const uint8_t bits = header16[15];
  frame->dispose_background = (uint8_t)(bits & 1u);
  frame->blend_source = (uint8_t)((bits & 2u) ? 1u : 0u);
  if (frame->width == 0u || frame->height == 0u) {
    return GIMG_ERR_CORRUPT;
  }
  return GIMG_OK;
}

GIMG_Result gimg_webp_decode_picture(const unsigned char * vp8, size_t vp8_size,
    const unsigned char * vp8l, size_t vp8l_size, const unsigned char * alph,
    size_t alph_size, const GIMG_Allocator * alloc, GIMG_Raster ** out_raster) {
  if (!out_raster) {
    return GIMG_ERR_INTERNAL;
  }
  *out_raster = NULL;
  if (vp8l && !vp8) {
    GIMG_Result r =
        gimg_webp_vp8l_decode(vp8l, vp8l_size, alloc, out_raster);
    if (r != GIMG_OK) {
      return r;
    }
    // ALPH beside VP8L is unusual for still files; ANMF may still carry it.
    if (alph && alph_size > 0u) {
      const uint32_t w = gimg_raster_width(*out_raster);
      const uint32_t h = gimg_raster_height(*out_raster);
      uint8_t * alpha = NULL;
      r = gimg_webp_alpha_decode(alph, alph_size, w, h, alloc, &alpha);
      if (r != GIMG_OK) {
        gimg_raster_destroy(*out_raster);
        *out_raster = NULL;
        return r;
      }
      uint8_t * px = (uint8_t *)gimg_raster_pixels(*out_raster);
      const size_t stride = gimg_raster_stride_bytes(*out_raster);
      for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) {
          px[(size_t)y * stride + (size_t)x * 4u + 3u] =
              alpha[(size_t)y * w + x];
        }
      }
      gimg_free(alloc, alpha);
    }
    return GIMG_OK;
  }
  if (vp8 && !vp8l) {
    GIMG_Result r = gimg_webp_vp8_decode(vp8, vp8_size, alloc, out_raster);
    if (r != GIMG_OK) {
      return r;
    }
    if (alph && alph_size > 0u) {
      const uint32_t w = gimg_raster_width(*out_raster);
      const uint32_t h = gimg_raster_height(*out_raster);
      uint8_t * alpha = NULL;
      r = gimg_webp_alpha_decode(alph, alph_size, w, h, alloc, &alpha);
      if (r != GIMG_OK) {
        gimg_raster_destroy(*out_raster);
        *out_raster = NULL;
        return r;
      }
      uint8_t * px = (uint8_t *)gimg_raster_pixels(*out_raster);
      const size_t stride = gimg_raster_stride_bytes(*out_raster);
      for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) {
          px[(size_t)y * stride + (size_t)x * 4u + 3u] =
              alpha[(size_t)y * w + x];
        }
      }
      gimg_free(alloc, alpha);
    }
    return GIMG_OK;
  }
  return GIMG_ERR_UNSUPPORTED;
}

/** Non-premultiplied OVER, matching libwebp BlendPixelNonPremult (RGBA). */
static void webp_blend_pixel(uint8_t * dst, const uint8_t * src) {
  const uint8_t sa = src[3];
  if (sa == 0) {
    return;
  }
  if (sa == 255) {
    dst[0] = src[0];
    dst[1] = src[1];
    dst[2] = src[2];
    dst[3] = 255;
    return;
  }
  const uint8_t da = dst[3];
  const uint8_t dst_factor_a = (uint8_t)((da * (256u - sa)) >> 8);
  const uint8_t blend_a = (uint8_t)(sa + dst_factor_a);
  if (blend_a == 0) {
    dst[0] = dst[1] = dst[2] = dst[3] = 0;
    return;
  }
  const uint32_t scale = (1u << 24) / (uint32_t)blend_a;
  for (int c = 0; c < 3; ++c) {
    const uint32_t blend_unscaled =
        (uint32_t)src[c] * sa + (uint32_t)dst[c] * dst_factor_a;
    dst[c] = (uint8_t)((blend_unscaled * scale) >> 24);
  }
  dst[3] = blend_a;
}

static void webp_blit_frame(uint8_t * canvas, size_t canvas_stride,
    uint32_t canvas_w, uint32_t canvas_h, const uint8_t * frame,
    size_t frame_stride, uint32_t fx, uint32_t fy, uint32_t fw, uint32_t fh,
    int blend_source) {
  if (fx >= canvas_w || fy >= canvas_h) {
    return;
  }
  if (fw > canvas_w - fx) {
    fw = canvas_w - fx;
  }
  if (fh > canvas_h - fy) {
    fh = canvas_h - fy;
  }
  for (uint32_t y = 0; y < fh; ++y) {
    uint8_t * dst =
        canvas + (size_t)(fy + y) * canvas_stride + (size_t)fx * 4u;
    const uint8_t * src = frame + (size_t)y * frame_stride;
    if (blend_source) {
      memcpy(dst, src, (size_t)fw * 4u);
    }
    else {
      for (uint32_t x = 0; x < fw; ++x) {
        webp_blend_pixel(dst + (size_t)x * 4u, src + (size_t)x * 4u);
      }
    }
  }
}

static void webp_clear_rect(uint8_t * canvas, size_t canvas_stride,
    uint32_t canvas_w, uint32_t canvas_h, uint32_t fx, uint32_t fy, uint32_t fw,
    uint32_t fh) {
  if (fx >= canvas_w || fy >= canvas_h) {
    return;
  }
  if (fw > canvas_w - fx) {
    fw = canvas_w - fx;
  }
  if (fh > canvas_h - fy) {
    fh = canvas_h - fy;
  }
  for (uint32_t y = 0; y < fh; ++y) {
    memset(canvas + (size_t)(fy + y) * canvas_stride + (size_t)fx * 4u, 0,
        (size_t)fw * 4u);
  }
}

static GIMG_Result webp_decode_one_frame(const gimg_webp_doc_state_t * st,
    const gimg_webp_frame_t * fr, GIMG_Raster ** out) {
  const unsigned char * vp8 = NULL;
  size_t vp8_size = 0;
  const unsigned char * vp8l = NULL;
  size_t vp8l_size = 0;
  const unsigned char * alph = NULL;
  size_t alph_size = 0;
  if (fr->vp8_payload_size > 0u) {
    if (fr->vp8_payload_off + (size_t)fr->vp8_payload_size > st->file_size) {
      return GIMG_ERR_CORRUPT;
    }
    vp8 = st->file_bytes + fr->vp8_payload_off;
    vp8_size = fr->vp8_payload_size;
  }
  if (fr->vp8l_payload_size > 0u) {
    if (fr->vp8l_payload_off + (size_t)fr->vp8l_payload_size > st->file_size) {
      return GIMG_ERR_CORRUPT;
    }
    vp8l = st->file_bytes + fr->vp8l_payload_off;
    vp8l_size = fr->vp8l_payload_size;
  }
  if (fr->alph_payload_size > 0u) {
    if (fr->alph_payload_off + (size_t)fr->alph_payload_size > st->file_size) {
      return GIMG_ERR_CORRUPT;
    }
    alph = st->file_bytes + fr->alph_payload_off;
    alph_size = fr->alph_payload_size;
  }
  return gimg_webp_decode_picture(
      vp8, vp8_size, vp8l, vp8l_size, alph, alph_size, st->allocator, out);
}

GIMG_Result gimg_webp_decode_animation_frame(const gimg_webp_doc_state_t * st,
    size_t index, GIMG_Raster ** out_raster) {
  if (!st || !out_raster) {
    return GIMG_ERR_INTERNAL;
  }
  *out_raster = NULL;
  if (index >= st->frame_count) {
    return GIMG_ERR_UNSUPPORTED;
  }

  const uint32_t canvas_w = st->canvas_width;
  const uint32_t canvas_h = st->canvas_height;
  GIMG_Raster * canvas = NULL;
  GIMG_Result r = gimg_raster_create_with_allocator(st->allocator, canvas_w,
      canvas_h, &GIMG_PIXEL_RGBA8, GIMG_RASTER_OWNED, NULL, 0, &canvas);
  if (r != GIMG_OK) {
    return r;
  }
  uint8_t * canvas_px = (uint8_t *)gimg_raster_pixels(canvas);
  const size_t canvas_stride = gimg_raster_stride_bytes(canvas);
  memset(canvas_px, 0, canvas_stride * (size_t)canvas_h);

  for (size_t i = 0; i <= index; ++i) {
    const gimg_webp_frame_t * fr = &st->frames[i];
    if (i > 0u) {
      const gimg_webp_frame_t * prev = &st->frames[i - 1u];
      if (prev->dispose_background) {
        webp_clear_rect(canvas_px, canvas_stride, canvas_w, canvas_h, prev->x,
            prev->y, prev->width, prev->height);
      }
    }
    if ((uint64_t)fr->x + fr->width > canvas_w ||
        (uint64_t)fr->y + fr->height > canvas_h) {
      gimg_raster_destroy(canvas);
      return GIMG_ERR_CORRUPT;
    }
    GIMG_Raster * frame_ras = NULL;
    r = webp_decode_one_frame(st, fr, &frame_ras);
    if (r != GIMG_OK) {
      gimg_raster_destroy(canvas);
      return r;
    }
    // Frame bitstream size must match the ANMF rectangle; otherwise the blit
    // would read past the decoded buffer or leave an incomplete patch.
    if (gimg_raster_width(frame_ras) != fr->width ||
        gimg_raster_height(frame_ras) != fr->height) {
      gimg_raster_destroy(frame_ras);
      gimg_raster_destroy(canvas);
      return GIMG_ERR_CORRUPT;
    }
    webp_blit_frame(canvas_px, canvas_stride, canvas_w, canvas_h,
        (const uint8_t *)gimg_raster_pixels(frame_ras),
        gimg_raster_stride_bytes(frame_ras), fr->x, fr->y, fr->width,
        fr->height, fr->blend_source);
    gimg_raster_destroy(frame_ras);
  }

  *out_raster = canvas;
  return GIMG_OK;
}

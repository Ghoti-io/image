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
 *
 * Portions derived from libwebp 1.5.0 (Copyright (c) 2010, Google Inc.).
 */

/**
 * @file
 *
 * VP8 keyframe macroblock loop, tokens, reconstruct; public
 * gimg_webp_vp8_decode. Fancy YUV→RGBA matching dwebp.
 */

#define HAVE_CONFIG_H
#include "vp8ref/src/webp/config.h"

#ifdef __GNUC__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wconversion"
#endif

#include "vp8ref/src/dec/tree_dec.inc"
#include "vp8ref/src/dec/frame_dec.inc"
#include "vp8ref/src/dec/vp8_dec.inc"
#include "vp8ref/src/dec/alpha_stub.inc"

#ifdef __GNUC__
#pragma GCC diagnostic pop
#endif

#include <ghoti.io/image/macros.h>
#include <string.h>

#include "../../core/alloc_internal.h"
#include "vp8ref/src/dec/vp8_dec.h"
#include "vp8ref/src/dsp/dsp.h"
#include "vp8ref/src/dsp/yuv.h"
#include "webp_internal.h"

typedef struct {
  const GIMG_Allocator * alloc;
  int width;
  int height;
  uint8_t * rgba;
  size_t rgba_stride;
  uint8_t * tmp_y;
  uint8_t * tmp_u;
  uint8_t * tmp_v;
  int failed;
} gimg_vp8_out_t;

static int gimg_vp8_setup(VP8Io * io) {
  gimg_vp8_out_t * o = (gimg_vp8_out_t *)io->opaque;
  o->width = io->width;
  o->height = io->height;
  o->rgba_stride = (size_t)io->width * 4u;
  o->rgba = (uint8_t *)gimg_calloc(
      o->alloc, (size_t)io->height, o->rgba_stride);
  o->tmp_y = (uint8_t *)gimg_calloc(o->alloc, (size_t)io->width + 16u, 1u);
  o->tmp_u =
      (uint8_t *)gimg_calloc(o->alloc, (size_t)(io->width + 1) / 2u + 16u, 1u);
  o->tmp_v =
      (uint8_t *)gimg_calloc(o->alloc, (size_t)(io->width + 1) / 2u + 16u, 1u);
  if (!o->rgba || !o->tmp_y || !o->tmp_u || !o->tmp_v) {
    o->failed = 1;
    return 0;
  }
  io->fancy_upsampling = 1;
  WebPInitUpsamplers();
  return 1;
}

/** EmitFancyRGB from libwebp io_dec.c, writing opaque RGBA. */
static int gimg_vp8_put(const VP8Io * io) {
  gimg_vp8_out_t * o = (gimg_vp8_out_t *)io->opaque;
  WebPUpsampleLinePairFunc upsample = WebPUpsamplers[MODE_RGBA];
  const uint8_t * cur_y = io->y;
  const uint8_t * cur_u = io->u;
  const uint8_t * cur_v = io->v;
  const uint8_t * top_u = o->tmp_u;
  const uint8_t * top_v = o->tmp_v;
  int y = io->mb_y;
  const int y_end = io->mb_y + io->mb_h;
  const int mb_w = io->mb_w;
  const int uv_w = (mb_w + 1) / 2;
  const int stride = (int)o->rgba_stride;
  uint8_t * dst = o->rgba + (size_t)io->mb_y * (size_t)stride;

  if (y == 0) {
    upsample(cur_y, NULL, cur_u, cur_v, cur_u, cur_v, dst, NULL, mb_w);
  } else {
    upsample(o->tmp_y, cur_y, top_u, top_v, cur_u, cur_v, dst - stride, dst,
        mb_w);
  }
  for (; y + 2 < y_end; y += 2) {
    top_u = cur_u;
    top_v = cur_v;
    cur_u += io->uv_stride;
    cur_v += io->uv_stride;
    dst += 2 * stride;
    cur_y += 2 * io->y_stride;
    upsample(cur_y - io->y_stride, cur_y, top_u, top_v, cur_u, cur_v,
        dst - stride, dst, mb_w);
  }
  cur_y += io->y_stride;
  if (io->crop_top + y_end < io->crop_bottom) {
    memcpy(o->tmp_y, cur_y, (size_t)mb_w);
    memcpy(o->tmp_u, cur_u, (size_t)uv_w);
    memcpy(o->tmp_v, cur_v, (size_t)uv_w);
  } else if (!(y_end & 1)) {
    upsample(cur_y, NULL, cur_u, cur_v, cur_u, cur_v, dst + stride, NULL, mb_w);
  }
  return 1;
}

static void gimg_vp8_teardown(const VP8Io * io) {
  (void)io;
}

static void gimg_vp8_out_clear(gimg_vp8_out_t * o) {
  if (!o) {
    return;
  }
  gimg_free(o->alloc, o->rgba);
  gimg_free(o->alloc, o->tmp_y);
  gimg_free(o->alloc, o->tmp_u);
  gimg_free(o->alloc, o->tmp_v);
  o->rgba = NULL;
  o->tmp_y = NULL;
  o->tmp_u = NULL;
  o->tmp_v = NULL;
}

GIMG_Result gimg_webp_vp8_decode(const unsigned char * data, size_t size,
    const GIMG_Allocator * alloc, GIMG_Raster ** out_raster) {
  if (out_raster) {
    *out_raster = NULL;
  }
  if (!data || !out_raster || size < 10u) {
    return GIMG_ERR_CORRUPT;
  }
  alloc = gimg_alloc_or_default(alloc);

  gimg_vp8_out_t out;
  memset(&out, 0, sizeof(out));
  out.alloc = alloc;

  VP8Io io;
  if (!VP8InitIo(&io)) {
    return GIMG_ERR_INTERNAL;
  }
  io.data = data;
  io.data_size = size;
  io.opaque = &out;
  io.setup = gimg_vp8_setup;
  io.put = gimg_vp8_put;
  io.teardown = gimg_vp8_teardown;

  VP8Decoder * dec = VP8New();
  if (!dec) {
    return GIMG_ERR_OOM;
  }
  const int ok = VP8Decode(dec, &io);
  const VP8StatusCode st = VP8Status(dec);
  VP8Delete(dec);

  if (!ok || out.failed || !out.rgba) {
    gimg_vp8_out_clear(&out);
    if (st == VP8_STATUS_OUT_OF_MEMORY) {
      return GIMG_ERR_OOM;
    }
    if (st == VP8_STATUS_UNSUPPORTED_FEATURE) {
      return GIMG_ERR_UNSUPPORTED;
    }
    return GIMG_ERR_CORRUPT;
  }

  GIMG_Raster * raster = NULL;
  GIMG_Result r = gimg_raster_create_with_allocator(alloc, (uint32_t)out.width,
      (uint32_t)out.height, &GIMG_PIXEL_RGBA8, GIMG_RASTER_OWNED, NULL, 0,
      &raster);
  if (r != GIMG_OK) {
    gimg_vp8_out_clear(&out);
    return r;
  }
  uint8_t * dst = (uint8_t *)gimg_raster_pixels(raster);
  const size_t dst_stride = gimg_raster_stride_bytes(raster);
  for (int y = 0; y < out.height; ++y) {
    memcpy(dst + (size_t)y * dst_stride, out.rgba + (size_t)y * out.rgba_stride,
        (size_t)out.width * 4u);
  }
  gimg_vp8_out_clear(&out);
  *out_raster = raster;
  return GIMG_OK;
}

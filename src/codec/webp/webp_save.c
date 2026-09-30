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
 * WebP Phase F/G: save a still as lossless VP8L (default) or stub lossy VP8
 * (optional VP8X + ICCP / EXIF / XMP). Lossy with non-opaque alpha is refused.
 * Multi-frame save is not implemented (first item only).
 */

#include <ghoti.io/image/macros.h>
#include <ghoti.io/image/meta.h>
#include <ghoti.io/image/raster.h>
#include <string.h>

#include "../../container/doc_internal.h"
#include "../../core/alloc_internal.h"
#include "../codec_internal.h"
#include "webp_internal.h"

static void write_u32le(unsigned char * p, uint32_t v) {
  p[0] = (unsigned char)(v & 0xffu);
  p[1] = (unsigned char)((v >> 8) & 0xffu);
  p[2] = (unsigned char)((v >> 16) & 0xffu);
  p[3] = (unsigned char)((v >> 24) & 0xffu);
}

static void write_u24le(unsigned char * p, uint32_t v) {
  p[0] = (unsigned char)(v & 0xffu);
  p[1] = (unsigned char)((v >> 8) & 0xffu);
  p[2] = (unsigned char)((v >> 16) & 0xffu);
}

typedef struct {
  unsigned char * data;
  size_t size;
  size_t cap;
  const GIMG_Allocator * alloc;
} webp_buf_t;

static int buf_reserve(webp_buf_t * b, size_t need) {
  size_t ncap;
  unsigned char * n;
  if (b->size + need <= b->cap) {
    return 1;
  }
  ncap = b->cap ? b->cap : 256u;
  while (ncap < b->size + need) {
    ncap *= 2u;
  }
  n = (unsigned char *)gimg_realloc(b->alloc, b->data, ncap);
  if (!n) {
    return 0;
  }
  b->data = n;
  b->cap = ncap;
  return 1;
}

static int buf_append(webp_buf_t * b, const void * p, size_t n) {
  if (!buf_reserve(b, n)) {
    return 0;
  }
  memcpy(b->data + b->size, p, n);
  b->size += n;
  return 1;
}

static int buf_append_chunk(webp_buf_t * b, uint32_t fourcc,
    const unsigned char * payload, size_t payload_size) {
  unsigned char hdr[8];
  write_u32le(hdr, fourcc);
  write_u32le(hdr + 4, (uint32_t)payload_size);
  if (!buf_append(b, hdr, 8u)) {
    return 0;
  }
  if (payload_size > 0u && !buf_append(b, payload, payload_size)) {
    return 0;
  }
  if (payload_size & 1u) {
    unsigned char pad = 0;
    if (!buf_append(b, &pad, 1u)) {
      return 0;
    }
  }
  return 1;
}

/** Sample one pixel as RGBA8 from formats this writer accepts. */
static int sample_rgba8(const GIMG_Raster * raster, uint32_t x, uint32_t y,
    uint8_t out[4]) {
  const GIMG_Pixel_Format * f = gimg_raster_format(raster);
  const uint8_t * pixels = (const uint8_t *)gimg_raster_pixels_const(raster);
  size_t stride = gimg_raster_stride_bytes(raster);
  const uint8_t * row;
  if (!f || !pixels || f->channel_type != GIMG_CHANNEL_UNORM ||
      f->bits_per_channel[0] != 8u) {
    return 0;
  }
  row = pixels + (size_t)y * stride;
  if (f->channel_model == GIMG_CHANNEL_GRAY && f->channel_count == 1) {
    uint8_t v = row[x];
    out[0] = out[1] = out[2] = v;
    out[3] = 255u;
    return 1;
  }
  if (f->channel_model == GIMG_CHANNEL_RGB && f->channel_count == 3) {
    const uint8_t * px = row + (size_t)x * 3u;
    out[0] = px[0];
    out[1] = px[1];
    out[2] = px[2];
    out[3] = 255u;
    return 1;
  }
  if ((f->channel_model == GIMG_CHANNEL_RGBA ||
          f->channel_model == GIMG_CHANNEL_RGB) &&
      f->channel_count == 4) {
    const uint8_t * px = row + (size_t)x * 4u;
    out[0] = px[0];
    out[1] = px[1];
    out[2] = px[2];
    out[3] = px[3];
    return 1;
  }
  return 0;
}

static GIMG_Result raster_to_rgba8(const GIMG_Raster * raster,
    const GIMG_Allocator * alloc, uint8_t ** out_rgba, size_t * out_stride,
    int * out_has_alpha) {
  uint32_t w = gimg_raster_width(raster);
  uint32_t h = gimg_raster_height(raster);
  size_t stride = (size_t)w * 4u;
  uint8_t * rgba;
  int has_alpha = 0;
  *out_rgba = NULL;
  *out_stride = 0;
  *out_has_alpha = 0;
  rgba = (uint8_t *)gimg_malloc(alloc, stride * (size_t)h);
  if (!rgba) {
    return GIMG_ERR_OOM;
  }
  for (uint32_t y = 0; y < h; ++y) {
    for (uint32_t x = 0; x < w; ++x) {
      uint8_t px[4];
      if (!sample_rgba8(raster, x, y, px)) {
        gimg_free(alloc, rgba);
        return GIMG_ERR_UNSUPPORTED;
      }
      if (px[3] != 255u) {
        has_alpha = 1;
      }
      memcpy(rgba + (size_t)y * stride + (size_t)x * 4u, px, 4u);
    }
  }
  *out_rgba = rgba;
  *out_stride = stride;
  *out_has_alpha = has_alpha;
  return GIMG_OK;
}

GIMG_Result gimg_webp_save(GIMG_Codec * codec, const GIMG_Doc * doc,
    GIMG_Stream * stream, const char * format_name,
    const GIMG_Save_Options * options, GIMG_Save_Report * report) {
  const GIMG_Allocator * alloc;
  GIMG_Item * item;
  GIMG_Raster * raster = NULL;
  int owned_raster = 0;
  uint8_t * rgba = NULL;
  size_t rgba_stride = 0;
  int has_alpha = 0;
  unsigned char * picture = NULL;
  size_t picture_size = 0;
  int is_lossy = 0;
  webp_buf_t file;
  GIMG_Result r = GIMG_OK;
  uint8_t lossless;
  uint8_t effort;
  uint8_t exact;
  GIMG_Meta_Policy meta_policy;
  unsigned char * iccp = NULL;
  size_t iccp_size = 0;
  unsigned char * exif = NULL;
  size_t exif_size = 0;
  unsigned char * xmp = NULL;
  size_t xmp_size = 0;
  int need_vp8x = 0;
  uint8_t vp8x_flags = 0;

  (void)format_name;
  if (!codec || !doc || !stream) {
    return GIMG_ERR_INTERNAL;
  }
  if (report) {
    memset(report, 0, sizeof(*report));
  }
  alloc = gimg_alloc_or_default(codec->allocator);

  lossless = options ? options->webp_lossless : (uint8_t)GIMG_WEBP_COMPRESS_LOSSLESS;
  effort = options ? options->webp_effort : 4u;
  exact = options ? options->webp_exact : 0u;
  meta_policy =
      options ? options->metadata_policy : GIMG_META_PRESERVE_ALL;
  is_lossy = (lossless == GIMG_WEBP_COMPRESS_LOSSY);

  if (gimg_doc_item_count(doc) < 1u) {
    return GIMG_ERR_UNSUPPORTED;
  }
  /* Animation write is not implemented: encode the first item as a still. */
  item = gimg_doc_item(doc, 0);
  if (!item) {
    return GIMG_ERR_INTERNAL;
  }

  if (gimg_item_raster(item)) {
    raster = gimg_item_raster(item);
  }
  else {
    r = gimg_item_decode(item, NULL, &raster);
    if (r != GIMG_OK) {
      return r;
    }
    owned_raster = 1;
  }

  r = raster_to_rgba8(raster, alloc, &rgba, &rgba_stride, &has_alpha);
  if (r != GIMG_OK) {
    goto Done;
  }

  if (is_lossy) {
    if (has_alpha) {
      /* Lossy ALPH encode is a follow-on; refuse non-opaque alpha for now. */
      r = GIMG_ERR_UNSUPPORTED;
      goto Done;
    }
    r = gimg_webp_vp8_encode(rgba, gimg_raster_width(raster),
        gimg_raster_height(raster), rgba_stride, (int)effort, alloc, &picture,
        &picture_size);
  }
  else {
    r = gimg_webp_vp8l_encode(rgba, gimg_raster_width(raster),
        gimg_raster_height(raster), rgba_stride, has_alpha, exact, (int)effort,
        alloc, &picture, &picture_size);
  }
  if (r != GIMG_OK) {
    goto Done;
  }

  if (meta_policy == GIMG_META_PRESERVE_ALL ||
      meta_policy == GIMG_META_KEEP_RAW_ONLY ||
      meta_policy == GIMG_META_NORMALIZE_EXIF ||
      meta_policy == GIMG_META_STRIP_GPS) {
    GIMG_Meta_Raw * raw = gimg_doc_meta_raw(doc);
    if (raw) {
      size_t sz = 0;
      if (gimg_meta_raw_get(raw, "webp", GIMG_WEBP_ICCP, NULL, &sz) ==
              GIMG_OK &&
          sz > 0u) {
        iccp = (unsigned char *)gimg_malloc(alloc, sz);
        if (!iccp) {
          r = GIMG_ERR_OOM;
          goto Done;
        }
        if (gimg_meta_raw_get(raw, "webp", GIMG_WEBP_ICCP, iccp, &sz) !=
            GIMG_OK) {
          gimg_free(alloc, iccp);
          iccp = NULL;
          sz = 0;
        }
        iccp_size = sz;
      }
      sz = 0;
      if (gimg_meta_raw_get(raw, "webp", GIMG_WEBP_EXIF, NULL, &sz) == GIMG_OK &&
          sz > 0u) {
        exif = (unsigned char *)gimg_malloc(alloc, sz);
        if (!exif) {
          r = GIMG_ERR_OOM;
          goto Done;
        }
        if (gimg_meta_raw_get(raw, "webp", GIMG_WEBP_EXIF, exif, &sz) !=
            GIMG_OK) {
          gimg_free(alloc, exif);
          exif = NULL;
          sz = 0;
        }
        exif_size = sz;
      }
      sz = 0;
      if (gimg_meta_raw_get(raw, "webp", GIMG_WEBP_XMP, NULL, &sz) == GIMG_OK &&
          sz > 0u) {
        xmp = (unsigned char *)gimg_malloc(alloc, sz);
        if (!xmp) {
          r = GIMG_ERR_OOM;
          goto Done;
        }
        if (gimg_meta_raw_get(raw, "webp", GIMG_WEBP_XMP, xmp, &sz) !=
            GIMG_OK) {
          gimg_free(alloc, xmp);
          xmp = NULL;
          sz = 0;
        }
        xmp_size = sz;
      }
    }
    if (iccp_size == 0u && meta_policy != GIMG_META_KEEP_RAW_ONLY) {
      const GCOL_Color_Info * ci = gimg_raster_color_info_const(raster);
      if (ci && ci->icc_bytes && ci->icc_size > 0u) {
        iccp = (unsigned char *)gimg_malloc(alloc, ci->icc_size);
        if (!iccp) {
          r = GIMG_ERR_OOM;
          goto Done;
        }
        memcpy(iccp, ci->icc_bytes, ci->icc_size);
        iccp_size = ci->icc_size;
      }
    }
  }
  if (meta_policy == GIMG_META_DROP_ALL ||
      meta_policy == GIMG_META_KEEP_COMMON_ONLY) {
    gimg_free(alloc, iccp);
    gimg_free(alloc, exif);
    gimg_free(alloc, xmp);
    iccp = NULL;
    iccp_size = 0;
    exif = NULL;
    exif_size = 0;
    xmp = NULL;
    xmp_size = 0;
  }

  if (iccp_size > 0u) {
    need_vp8x = 1;
    vp8x_flags |= (uint8_t)GIMG_WEBP_VP8X_ICCP;
  }
  if (exif_size > 0u) {
    need_vp8x = 1;
    vp8x_flags |= (uint8_t)GIMG_WEBP_VP8X_EXIF;
  }
  if (xmp_size > 0u) {
    need_vp8x = 1;
    vp8x_flags |= (uint8_t)GIMG_WEBP_VP8X_XMP;
  }
  if (has_alpha && need_vp8x) {
    vp8x_flags |= (uint8_t)GIMG_WEBP_VP8X_ALPHA;
  }

  memset(&file, 0, sizeof(file));
  file.alloc = alloc;
  {
    unsigned char riff[12];
    memcpy(riff, "RIFF", 4);
    write_u32le(riff + 4, 0); /* filled below */
    memcpy(riff + 8, "WEBP", 4);
    if (!buf_append(&file, riff, 12u)) {
      r = GIMG_ERR_OOM;
      goto Done;
    }
  }

  if (need_vp8x) {
    unsigned char vp8x[10];
    memset(vp8x, 0, sizeof(vp8x));
    vp8x[0] = vp8x_flags;
    write_u24le(vp8x + 4, gimg_raster_width(raster) - 1u);
    write_u24le(vp8x + 7, gimg_raster_height(raster) - 1u);
    if (!buf_append_chunk(&file, GIMG_WEBP_VP8X, vp8x, 10u)) {
      r = GIMG_ERR_OOM;
      goto Done;
    }
  }
  if (iccp_size > 0u &&
      !buf_append_chunk(&file, GIMG_WEBP_ICCP, iccp, iccp_size)) {
    r = GIMG_ERR_OOM;
    goto Done;
  }
  if (!buf_append_chunk(&file, is_lossy ? GIMG_WEBP_VP8 : GIMG_WEBP_VP8L,
          picture, picture_size)) {
    r = GIMG_ERR_OOM;
    goto Done;
  }
  if (exif_size > 0u &&
      !buf_append_chunk(&file, GIMG_WEBP_EXIF, exif, exif_size)) {
    r = GIMG_ERR_OOM;
    goto Done;
  }
  if (xmp_size > 0u && !buf_append_chunk(&file, GIMG_WEBP_XMP, xmp, xmp_size)) {
    r = GIMG_ERR_OOM;
    goto Done;
  }

  {
    uint32_t riff_size = (uint32_t)(file.size - 8u);
    write_u32le(file.data + 4, riff_size);
  }

  {
    size_t written = 0;
    r = gimg_stream_write(stream, file.data, file.size, &written);
    if (r == GIMG_OK && written != file.size) {
      r = GIMG_ERR_IO;
    }
    if (report && r == GIMG_OK) {
      report->bytes_written = written;
    }
  }

Done:
  gimg_free(alloc, file.data);
  gimg_free(alloc, picture);
  gimg_free(alloc, rgba);
  gimg_free(alloc, iccp);
  gimg_free(alloc, exif);
  gimg_free(alloc, xmp);
  if (owned_raster) {
    gimg_raster_destroy(raster);
  }
  return r;
}

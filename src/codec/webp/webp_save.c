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
 * WebP Phase F/G: save a still as lossless VP8L (default) or lossy VP8
 * (optional VP8X + ALPH + ICCP / EXIF / XMP). A lossy picture with alpha
 * stores the plane in ALPH, as VP8L when that is shorter than the raw
 * bytes, and premultiplies the colour the VP8 frame carries. A document
 * of several items is an animation:
 * each item is the canvas at that moment, and the frame written for
 * it is the rectangle that differs from what is already showing.
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

/** Straight alpha, kept exact. RGB is scaled by it so a transparent
 *  sample does not spend VP8 bits on colour the display will not show.
 *  Rounding is (c * a + 127) / 255, so 255 reproduces the sample. */
static void premultiply_rgba(uint8_t * rgba, uint32_t width, uint32_t height,
    size_t stride) {
  uint32_t y;
  uint32_t x;
  for (y = 0; y < height; ++y) {
    uint8_t * row = rgba + (size_t)y * stride;
    for (x = 0; x < width; ++x) {
      uint8_t * p = row + (size_t)x * 4u;
      const int a = (int)p[3];
      if (a == 255) {
        continue;
      }
      if (a == 0) {
        p[0] = 0;
        p[1] = 0;
        p[2] = 0;
        continue;
      }
      p[0] = (uint8_t)((p[0] * a + 127) / 255);
      p[1] = (uint8_t)((p[1] * a + 127) / 255);
      p[2] = (uint8_t)((p[2] * a + 127) / 255);
    }
  }
}

/** ALPH method 0, no filter, no level reduction. One header byte, then
 *  the straight alpha plane in row order. */
static GIMG_Result encode_alpha_raw(const uint8_t * rgba, uint32_t width,
    uint32_t height, size_t stride, const GIMG_Allocator * alloc,
    unsigned char ** out, size_t * out_size) {
  const size_t n = 1u + (size_t)width * (size_t)height;
  unsigned char * buf = (unsigned char *)gimg_malloc(alloc, n);
  uint32_t y;
  uint32_t x;
  if (!buf) {
    return GIMG_ERR_OOM;
  }
  buf[0] = 0;
  for (y = 0; y < height; ++y) {
    const uint8_t * row = rgba + (size_t)y * stride;
    for (x = 0; x < width; ++x) {
      buf[1u + (size_t)y * width + x] = row[(size_t)x * 4u + 3u];
    }
  }
  *out = buf;
  *out_size = n;
  return GIMG_OK;
}

/** Raw alpha, or a VP8L bitstream of the green channel when that is
 *  shorter. The decoder reads that green channel back as the plane.
 *  Method 1, no filter, no level reduction: one header byte, then the
 *  transform stream with the five-byte VP8L frame header left off. */
static GIMG_Result encode_alpha(const uint8_t * rgba, uint32_t width,
    uint32_t height, size_t stride, int effort, int exact,
    const GIMG_Allocator * alloc, unsigned char ** out, size_t * out_size) {
  unsigned char * raw = NULL;
  size_t raw_size = 0;
  uint8_t * gray;
  unsigned char * vp8l = NULL;
  size_t vp8l_size = 0;
  unsigned char * packed;
  size_t n;
  uint32_t y;
  uint32_t x;
  GIMG_Result r;

  r = encode_alpha_raw(rgba, width, height, stride, alloc, &raw, &raw_size);
  if (r != GIMG_OK) {
    return r;
  }
  n = (size_t)width * (size_t)height;
  gray = (uint8_t *)gimg_malloc(alloc, n * 4u);
  if (!gray) {
    *out = raw;
    *out_size = raw_size;
    return GIMG_OK;
  }
  for (y = 0; y < height; ++y) {
    const uint8_t * row = rgba + (size_t)y * stride;
    uint8_t * dst = gray + (size_t)y * (size_t)width * 4u;
    for (x = 0; x < width; ++x) {
      dst[(size_t)x * 4u] = 0;
      dst[(size_t)x * 4u + 1u] = row[(size_t)x * 4u + 3u];
      dst[(size_t)x * 4u + 2u] = 0;
      dst[(size_t)x * 4u + 3u] = 255u;
    }
  }
  r = gimg_webp_vp8l_encode(gray, width, height, (size_t)width * 4u, 0, exact,
      effort, alloc, &vp8l, &vp8l_size);
  gimg_free(alloc, gray);
  /* The ALPH method-1 body omits the 5-byte VP8L frame header and starts
   * at the transform bit. 8+14+14+1+3 = 40 bits. */
  if (r != GIMG_OK || vp8l == NULL || vp8l_size <= 5u ||
      (vp8l_size - 5u) + 1u >= raw_size) {
    gimg_free(alloc, vp8l);
    if (r == GIMG_ERR_OOM) {
      gimg_free(alloc, raw);
      return r;
    }
    *out = raw;
    *out_size = raw_size;
    return GIMG_OK;
  }
  vp8l_size -= 5u;
  packed = (unsigned char *)gimg_malloc(alloc, vp8l_size + 1u);
  if (!packed) {
    gimg_free(alloc, vp8l);
    gimg_free(alloc, raw);
    return GIMG_ERR_OOM;
  }
  packed[0] = 1;
  memcpy(packed + 1, vp8l + 5, vp8l_size);
  gimg_free(alloc, vp8l);
  gimg_free(alloc, raw);
  *out = packed;
  *out_size = vp8l_size + 1u;
  return GIMG_OK;
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

/** Item delay is a fraction. ANMF wants milliseconds, and the loader
 *  stores that count back in a uint16, so the written value stops at
 *  65535. A denominator of 0 is 100, the APNG reading of a missing den. */
static uint32_t frame_duration_ms(const GIMG_Item * item) {
  uint16_t num = 0;
  uint16_t den = 0;
  uint32_t den_u;
  uint32_t ms;
  gimg_item_frame_delay(item, &num, &den);
  den_u = den == 0u ? 100u : (uint32_t)den;
  ms = ((uint32_t)num * 1000u) / den_u;
  if (ms > 65535u) {
    ms = 65535u;
  }
  return ms;
}

/** One ANMF: the 16-byte header, then the frame's own bitstream chunks.
 *  @a x and @a y are canvas pixels and even, because the file stores
 *  half-pixels. Blending is off, so the rectangle replaces what is there. */
static int append_anmf(webp_buf_t * file, uint32_t x, uint32_t y,
    uint32_t width, uint32_t height, uint32_t duration_ms,
    int dispose_background, int is_lossy, const unsigned char * alph,
    size_t alph_size, const unsigned char * picture, size_t picture_size) {
  webp_buf_t payload;
  unsigned char hdr[16];
  int ok;
  memset(&payload, 0, sizeof(payload));
  payload.alloc = file->alloc;
  memset(hdr, 0, sizeof(hdr));
  write_u24le(hdr + 0, x / 2u);
  write_u24le(hdr + 3, y / 2u);
  write_u24le(hdr + 6, width - 1u);
  write_u24le(hdr + 9, height - 1u);
  write_u24le(hdr + 12, duration_ms);
  /* Bit 1 set is "do not blend". Bit 0 is dispose to background. */
  hdr[15] = (unsigned char)(2u | (dispose_background ? 1u : 0u));
  ok = buf_append(&payload, hdr, sizeof(hdr));
  if (ok && alph_size > 0u) {
    ok = buf_append_chunk(&payload, GIMG_WEBP_ALPH, alph, alph_size);
  }
  if (ok) {
    ok = buf_append_chunk(&payload, is_lossy ? GIMG_WEBP_VP8 : GIMG_WEBP_VP8L,
        picture, picture_size);
  }
  if (ok) {
    ok = buf_append_chunk(file, GIMG_WEBP_ANMF, payload.data, payload.size);
  }
  gimg_free(file->alloc, payload.data);
  return ok;
}

static GIMG_Result take_item_raster(GIMG_Item * item, GIMG_Raster ** out,
    int * owned) {
  *out = NULL;
  *owned = 0;
  if (!item) {
    return GIMG_ERR_INTERNAL;
  }
  if (gimg_item_raster(item)) {
    *out = gimg_item_raster(item);
    return GIMG_OK;
  }
  *owned = 1;
  return gimg_item_decode(item, NULL, out);
}

/** Bounding box of pixels that differ, grown so the origin is even.
 *  An unchanged frame still needs a duration, so it becomes one pixel
 *  at the origin. */
static void changed_rect(const uint8_t * desired, size_t desired_stride,
    const uint8_t * screen, size_t screen_stride, uint32_t width,
    uint32_t height, uint32_t * out_x, uint32_t * out_y, uint32_t * out_w,
    uint32_t * out_h) {
  uint32_t min_x = width;
  uint32_t min_y = height;
  uint32_t max_x = 0;
  uint32_t max_y = 0;
  int any = 0;
  uint32_t y;
  uint32_t x;
  for (y = 0; y < height; ++y) {
    const uint8_t * drow = desired + (size_t)y * desired_stride;
    const uint8_t * srow = screen + (size_t)y * screen_stride;
    for (x = 0; x < width; ++x) {
      if (memcmp(drow + (size_t)x * 4u, srow + (size_t)x * 4u, 4u) == 0) {
        continue;
      }
      if (!any || x < min_x) {
        min_x = x;
      }
      if (!any || y < min_y) {
        min_y = y;
      }
      if (x > max_x) {
        max_x = x;
      }
      if (y > max_y) {
        max_y = y;
      }
      any = 1;
    }
  }
  if (!any) {
    *out_x = 0;
    *out_y = 0;
    *out_w = 1;
    *out_h = 1;
    return;
  }
  if (min_x & 1u) {
    min_x--;
  }
  if (min_y & 1u) {
    min_y--;
  }
  *out_x = min_x;
  *out_y = min_y;
  *out_w = (max_x + 1u) - min_x;
  *out_h = (max_y + 1u) - min_y;
}

static uint8_t * copy_rect(const uint8_t * src, size_t src_stride, uint32_t x,
    uint32_t y, uint32_t w, uint32_t h, const GIMG_Allocator * alloc,
    int * out_has_alpha) {
  size_t stride = (size_t)w * 4u;
  uint8_t * dst = (uint8_t *)gimg_malloc(alloc, stride * (size_t)h);
  uint32_t row;
  uint32_t col;
  int has_alpha = 0;
  if (!dst) {
    return NULL;
  }
  for (row = 0; row < h; ++row) {
    const uint8_t * from =
        src + (size_t)(y + row) * src_stride + (size_t)x * 4u;
    memcpy(dst + (size_t)row * stride, from, stride);
    for (col = 0; col < w; ++col) {
      if (from[col * 4u + 3u] != 255u) {
        has_alpha = 1;
      }
    }
  }
  *out_has_alpha = has_alpha;
  return dst;
}

static void paint_canvas(uint8_t * screen, size_t screen_stride,
    const uint8_t * desired, size_t desired_stride, uint32_t width,
    uint32_t height) {
  uint32_t y;
  for (y = 0; y < height; ++y) {
    memcpy(screen + (size_t)y * screen_stride,
        desired + (size_t)y * desired_stride, (size_t)width * 4u);
  }
}

static void clear_rect(uint8_t * screen, size_t screen_stride, uint32_t x,
    uint32_t y, uint32_t w, uint32_t h) {
  uint32_t row;
  for (row = 0; row < h; ++row) {
    memset(screen + (size_t)(y + row) * screen_stride + (size_t)x * 4u, 0,
        (size_t)w * 4u);
  }
}

/**
 * Each item is the canvas at that moment. The written rectangle is the
 * part that differs from what a player would already be showing, grown
 * so its origin is even. WebP has no dispose-to-previous, so that op is
 * refused, as is a frame that is not the canvas size. The ANIM chunk
 * always carries a loop count and a background: the format has no way
 * to omit either, and an absent one is written as 0 (repeat forever,
 * transparent black).
 */
static GIMG_Result webp_save_animation(GIMG_Codec * codec, const GIMG_Doc * doc,
    GIMG_Stream * stream, const GIMG_Save_Options * options,
    GIMG_Save_Report * report) {
  const GIMG_Allocator * alloc = gimg_alloc_or_default(codec->allocator);
  const size_t nitems = gimg_doc_item_count(doc);
  const int is_lossy =
      (options ? options->webp_lossless : (uint8_t)GIMG_WEBP_COMPRESS_LOSSLESS) ==
      GIMG_WEBP_COMPRESS_LOSSY;
  const int effort = (int)(options ? options->webp_effort : 4u);
  const uint8_t exact = options ? options->webp_exact : 0u;
  const GIMG_Meta_Policy meta_policy =
      options ? options->metadata_policy : GIMG_META_PRESERVE_ALL;
  webp_buf_t anmf;
  webp_buf_t file;
  GIMG_Raster * first = NULL;
  int first_owned = 0;
  uint32_t canvas_w = 0;
  uint32_t canvas_h = 0;
  uint8_t * screen = NULL;
  size_t screen_stride = 0;
  int any_alpha = 0;
  unsigned char * iccp = NULL;
  size_t iccp_size = 0;
  unsigned char * exif = NULL;
  size_t exif_size = 0;
  unsigned char * xmp = NULL;
  size_t xmp_size = 0;
  uint8_t vp8x_flags = (uint8_t)GIMG_WEBP_VP8X_ANIMATION;
  GIMG_Result r = GIMG_OK;
  size_t i;

  memset(&anmf, 0, sizeof(anmf));
  memset(&file, 0, sizeof(file));
  anmf.alloc = alloc;
  file.alloc = alloc;

  if (nitems > GIMG_WEBP_DEFAULT_MAX_FRAMES) {
    return GIMG_ERR_LIMIT;
  }

  for (i = 0; i < nitems; ++i) {
    GIMG_Item * item = gimg_doc_item(doc, i);
    GIMG_Raster * raster = NULL;
    int owned = 0;
    uint8_t * rgba = NULL;
    size_t rgba_stride = 0;
    int has_alpha = 0;
    unsigned char * picture = NULL;
    size_t picture_size = 0;
    unsigned char * alph = NULL;
    size_t alph_size = 0;
    uint32_t w;
    uint32_t h;
    GIMG_Dispose_Op dispose;

    if (!item) {
      r = GIMG_ERR_INTERNAL;
      goto Done;
    }
    dispose = gimg_item_dispose_op(item);
    if (dispose == GIMG_DISPOSE_PREVIOUS) {
      r = GIMG_ERR_UNSUPPORTED;
      goto Done;
    }
    r = take_item_raster(item, &raster, &owned);
    if (r != GIMG_OK) {
      goto Done;
    }
    w = gimg_raster_width(raster);
    h = gimg_raster_height(raster);
    if (w == 0u || h == 0u || w > 0x1000000u || h > 0x1000000u) {
      r = GIMG_ERR_UNSUPPORTED;
      if (owned) {
        gimg_raster_destroy(raster);
      }
      goto Done;
    }
    if (i == 0u) {
      canvas_w = w;
      canvas_h = h;
      first = raster;
      first_owned = owned;
      owned = 0;
    }
    else if (w != canvas_w || h != canvas_h) {
      r = GIMG_ERR_UNSUPPORTED;
      if (owned) {
        gimg_raster_destroy(raster);
      }
      goto Done;
    }

    r = raster_to_rgba8(raster, alloc, &rgba, &rgba_stride, &has_alpha);
    (void)has_alpha;
    if (r != GIMG_OK) {
      if (owned) {
        gimg_raster_destroy(raster);
      }
      goto Done;
    }
    if (i == 0u) {
      screen_stride = (size_t)canvas_w * 4u;
      screen = (uint8_t *)gimg_malloc(
          alloc, screen_stride * (size_t)canvas_h);
      if (!screen) {
        gimg_free(alloc, rgba);
        r = GIMG_ERR_OOM;
        goto Done;
      }
      memset(screen, 0, screen_stride * (size_t)canvas_h);
    }
    {
      uint32_t rx = 0;
      uint32_t ry = 0;
      uint32_t rw = 0;
      uint32_t rh = 0;
      uint8_t * patch;
      int patch_alpha = 0;
      changed_rect(rgba, rgba_stride, screen, screen_stride, canvas_w,
          canvas_h, &rx, &ry, &rw, &rh);
      patch = copy_rect(rgba, rgba_stride, rx, ry, rw, rh, alloc, &patch_alpha);
      if (!patch) {
        gimg_free(alloc, rgba);
        if (owned) {
          gimg_raster_destroy(raster);
        }
        r = GIMG_ERR_OOM;
        goto Done;
      }
      if (patch_alpha) {
        any_alpha = 1;
      }
      if (is_lossy) {
        if (patch_alpha) {
          r = encode_alpha(patch, rw, rh, (size_t)rw * 4u, effort, (int)exact,
              alloc, &alph, &alph_size);
          if (r == GIMG_OK) {
            premultiply_rgba(patch, rw, rh, (size_t)rw * 4u);
          }
        }
        if (r == GIMG_OK) {
          r = gimg_webp_vp8_encode(patch, rw, rh, (size_t)rw * 4u, effort,
              alloc, &picture, &picture_size);
        }
      }
      else {
        r = gimg_webp_vp8l_encode(patch, rw, rh, (size_t)rw * 4u, patch_alpha,
            exact, effort, alloc, &picture, &picture_size);
      }
      gimg_free(alloc, patch);
      if (r != GIMG_OK) {
        gimg_free(alloc, rgba);
        gimg_free(alloc, alph);
        gimg_free(alloc, picture);
        if (owned) {
          gimg_raster_destroy(raster);
        }
        goto Done;
      }
      paint_canvas(screen, screen_stride, rgba, rgba_stride, canvas_w,
          canvas_h);
      if (dispose == GIMG_DISPOSE_BACKGROUND) {
        clear_rect(screen, screen_stride, rx, ry, rw, rh);
      }
      gimg_free(alloc, rgba);
      rgba = NULL;
      if (!append_anmf(&anmf, rx, ry, rw, rh, frame_duration_ms(item),
              dispose == GIMG_DISPOSE_BACKGROUND, is_lossy, alph, alph_size,
              picture, picture_size)) {
        r = GIMG_ERR_OOM;
      }
    }
    gimg_free(alloc, alph);
    gimg_free(alloc, picture);
    if (owned) {
      gimg_raster_destroy(raster);
    }
    if (r != GIMG_OK) {
      goto Done;
    }
  }

  if (meta_policy == GIMG_META_PRESERVE_ALL ||
      meta_policy == GIMG_META_KEEP_RAW_ONLY ||
      meta_policy == GIMG_META_NORMALIZE_EXIF ||
      meta_policy == GIMG_META_STRIP_GPS) {
    GIMG_Meta_Raw * raw = gimg_doc_meta_raw(doc);
    if (raw) {
      size_t sz = 0;
      if (gimg_meta_raw_get(raw, "webp", GIMG_WEBP_ICCP, NULL, &sz) == GIMG_OK &&
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
        if (gimg_meta_raw_get(raw, "webp", GIMG_WEBP_XMP, xmp, &sz) != GIMG_OK) {
          gimg_free(alloc, xmp);
          xmp = NULL;
          sz = 0;
        }
        xmp_size = sz;
      }
    }
    if (iccp_size == 0u && meta_policy != GIMG_META_KEEP_RAW_ONLY && first) {
      const GCOL_Color_Info * ci = gimg_raster_color_info_const(first);
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

  if (any_alpha) {
    vp8x_flags = (uint8_t)(vp8x_flags | GIMG_WEBP_VP8X_ALPHA);
  }
  if (iccp_size > 0u) {
    vp8x_flags = (uint8_t)(vp8x_flags | GIMG_WEBP_VP8X_ICCP);
  }
  if (exif_size > 0u) {
    vp8x_flags = (uint8_t)(vp8x_flags | GIMG_WEBP_VP8X_EXIF);
  }
  if (xmp_size > 0u) {
    vp8x_flags = (uint8_t)(vp8x_flags | GIMG_WEBP_VP8X_XMP);
  }

  {
    unsigned char riff[12];
    unsigned char vp8x[10];
    unsigned char anim[6];
    uint32_t loop = 0;
    uint8_t bg[4] = { 0, 0, 0, 0 };
    memcpy(riff, "RIFF", 4);
    write_u32le(riff + 4, 0);
    memcpy(riff + 8, "WEBP", 4);
    if (!buf_append(&file, riff, 12u)) {
      r = GIMG_ERR_OOM;
      goto Done;
    }
    memset(vp8x, 0, sizeof(vp8x));
    vp8x[0] = vp8x_flags;
    write_u24le(vp8x + 4, canvas_w - 1u);
    write_u24le(vp8x + 7, canvas_h - 1u);
    if (!buf_append_chunk(&file, GIMG_WEBP_VP8X, vp8x, 10u)) {
      r = GIMG_ERR_OOM;
      goto Done;
    }
    if (iccp_size > 0u &&
        !buf_append_chunk(&file, GIMG_WEBP_ICCP, iccp, iccp_size)) {
      r = GIMG_ERR_OOM;
      goto Done;
    }
    if (gimg_doc_loop_count(doc, &loop) == 0) {
      loop = 0;
    }
    if (loop > 65535u) {
      loop = 65535u;
    }
    (void)gimg_doc_background_color(doc, bg);
    anim[0] = bg[2];
    anim[1] = bg[1];
    anim[2] = bg[0];
    anim[3] = bg[3];
    anim[4] = (unsigned char)(loop & 0xffu);
    anim[5] = (unsigned char)((loop >> 8) & 0xffu);
    if (!buf_append_chunk(&file, GIMG_WEBP_ANIM, anim, 6u)) {
      r = GIMG_ERR_OOM;
      goto Done;
    }
    if (!buf_append(&file, anmf.data, anmf.size)) {
      r = GIMG_ERR_OOM;
      goto Done;
    }
    if (exif_size > 0u &&
        !buf_append_chunk(&file, GIMG_WEBP_EXIF, exif, exif_size)) {
      r = GIMG_ERR_OOM;
      goto Done;
    }
    if (xmp_size > 0u &&
        !buf_append_chunk(&file, GIMG_WEBP_XMP, xmp, xmp_size)) {
      r = GIMG_ERR_OOM;
      goto Done;
    }
    write_u32le(file.data + 4, (uint32_t)(file.size - 8u));
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
  gimg_free(alloc, anmf.data);
  gimg_free(alloc, screen);
  gimg_free(alloc, iccp);
  gimg_free(alloc, exif);
  gimg_free(alloc, xmp);
  if (first_owned) {
    gimg_raster_destroy(first);
  }
  return r;
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
  unsigned char * alph = NULL;
  size_t alph_size = 0;
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
  /* Done frees file.data. Several error returns reach it before the RIFF
   * buffer exists, so the pointer has to be null from here on. */
  memset(&file, 0, sizeof(file));
  file.alloc = alloc;

  lossless = options ? options->webp_lossless : (uint8_t)GIMG_WEBP_COMPRESS_LOSSLESS;
  effort = options ? options->webp_effort : 4u;
  exact = options ? options->webp_exact : 0u;
  meta_policy =
      options ? options->metadata_policy : GIMG_META_PRESERVE_ALL;
  is_lossy = (lossless == GIMG_WEBP_COMPRESS_LOSSY);

  if (gimg_doc_item_count(doc) < 1u) {
    return GIMG_ERR_UNSUPPORTED;
  }
  if (gimg_doc_item_count(doc) > 1u) {
    return webp_save_animation(codec, doc, stream, options, report);
  }
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
      r = encode_alpha(rgba, gimg_raster_width(raster),
          gimg_raster_height(raster), rgba_stride, (int)effort, (int)exact,
          alloc, &alph, &alph_size);
      if (r != GIMG_OK) {
        goto Done;
      }
      premultiply_rgba(rgba, gimg_raster_width(raster),
          gimg_raster_height(raster), rgba_stride);
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
  if (is_lossy && has_alpha) {
    need_vp8x = 1;
    vp8x_flags |= (uint8_t)GIMG_WEBP_VP8X_ALPHA;
  }
  else if (has_alpha && need_vp8x) {
    vp8x_flags |= (uint8_t)GIMG_WEBP_VP8X_ALPHA;
  }

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
  if (alph_size > 0u &&
      !buf_append_chunk(&file, GIMG_WEBP_ALPH, alph, alph_size)) {
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
  gimg_free(alloc, alph);
  gimg_free(alloc, rgba);
  gimg_free(alloc, iccp);
  gimg_free(alloc, exif);
  gimg_free(alloc, xmp);
  if (owned_raster) {
    gimg_raster_destroy(raster);
  }
  return r;
}

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
 * ICO/CUR save: directory of DIB and/or PNG payloads.
 *
 * ico_payload AUTO writes PNG when either dimension exceeds 128 or the source
 * has non-trivial alpha; otherwise DIB. FRAME items are refused.
 */

#include <ghoti.io/image/macros.h>
#include <string.h>

#include "../../container/doc_internal.h"
#include "../../core/alloc_internal.h"
#include "../codec_internal.h"
#include "ico_internal.h"

static void ico_write_u16(unsigned char * p, uint16_t v) {
  p[0] = (unsigned char)(v & 0xFFu);
  p[1] = (unsigned char)((v >> 8) & 0xFFu);
}

static void ico_write_u32(unsigned char * p, uint32_t v) {
  p[0] = (unsigned char)(v & 0xFFu);
  p[1] = (unsigned char)((v >> 8) & 0xFFu);
  p[2] = (unsigned char)((v >> 16) & 0xFFu);
  p[3] = (unsigned char)((v >> 24) & 0xFFu);
}

/** Read one pixel as RGBA8 from a format this writer accepts. */
static void ico_sample(const GIMG_Pixel_Format * f, const uint8_t * row,
    uint32_t x, uint8_t out[4]) {
  if (f->channel_model == GIMG_CHANNEL_GRAY) {
    uint8_t v = row[x];
    out[0] = v;
    out[1] = v;
    out[2] = v;
    out[3] = 255u;
    return;
  }
  if (f->channel_model == GIMG_CHANNEL_RGB && f->channel_count == 3) {
    const uint8_t * px = row + (size_t)x * 3u;
    out[0] = px[0];
    out[1] = px[1];
    out[2] = px[2];
    out[3] = 255u;
    return;
  }
  const uint8_t * px = row + (size_t)x * 4u;
  out[0] = px[0];
  out[1] = px[1];
  out[2] = px[2];
  out[3] = px[3];
}

static int ico_format_supported(const GIMG_Pixel_Format * fmt) {
  if (!fmt || fmt->channel_type != GIMG_CHANNEL_UNORM) {
    return 0;
  }
  if (fmt->bits_per_channel[0] != 8u) {
    return 0;
  }
  if (fmt->channel_model == GIMG_CHANNEL_GRAY && fmt->channel_count == 1) {
    return 1;
  }
  if (fmt->channel_model == GIMG_CHANNEL_RGB && fmt->channel_count == 3) {
    return 1;
  }
  if (fmt->channel_model == GIMG_CHANNEL_RGBA && fmt->channel_count == 4) {
    return 1;
  }
  return 0;
}

static int ico_has_nontrivial_alpha(const GIMG_Raster * raster) {
  const GIMG_Pixel_Format * fmt = gimg_raster_format(raster);
  if (!fmt || fmt->channel_model != GIMG_CHANNEL_RGBA) {
    return 0;
  }
  const uint8_t * pixels =
      (const uint8_t *)gimg_raster_pixels_const(raster);
  size_t stride = gimg_raster_stride_bytes(raster);
  uint32_t w = gimg_raster_width(raster);
  uint32_t h = gimg_raster_height(raster);
  for (uint32_t y = 0; y < h; y++) {
    const uint8_t * row = pixels + (size_t)y * stride;
    for (uint32_t x = 0; x < w; x++) {
      uint8_t rgba[4];
      ico_sample(fmt, row, x, rgba);
      if (rgba[3] != 255u) {
        return 1; // any transparency → prefer PNG under AUTO
      }
    }
  }
  return 0;
}

static int ico_choose_png(const GIMG_Raster * raster, uint8_t payload_opt) {
  if (payload_opt == GIMG_ICO_PAYLOAD_PNG) {
    return 1;
  }
  if (payload_opt == GIMG_ICO_PAYLOAD_DIB) {
    return 0;
  }
  // AUTO
  uint32_t w = gimg_raster_width(raster);
  uint32_t h = gimg_raster_height(raster);
  if (w > 128u || h > 128u) {
    return 1;
  }
  return ico_has_nontrivial_alpha(raster);
}

/** Encode a supported 8-bit raster as a 32-bpp BI_RGB DIB with AND mask. */
static GIMG_Result ico_encode_dib(const GIMG_Allocator * alloc,
    const GIMG_Raster * raster, unsigned char ** out_bytes, size_t * out_size) {
  const GIMG_Pixel_Format * fmt = gimg_raster_format(raster);
  if (!ico_format_supported(fmt)) {
    return GIMG_ERR_UNSUPPORTED;
  }
  uint32_t w = gimg_raster_width(raster);
  uint32_t h = gimg_raster_height(raster);
  size_t xor_stride = ((size_t)w * 4u + 3u) & ~(size_t)3u;
  size_t mask_stride = ((size_t)w + 31u) / 32u * 4u;
  size_t xor_bytes = xor_stride * (size_t)h;
  size_t mask_bytes = mask_stride * (size_t)h;
  size_t total = 40u + xor_bytes + mask_bytes;

  unsigned char * buf = (unsigned char *)gimg_calloc(alloc, 1u, total);
  if (!buf) {
    return GIMG_ERR_OOM;
  }
  // BITMAPINFOHEADER
  ico_write_u32(buf + 0, 40u);
  ico_write_u32(buf + 4, w);
  ico_write_u32(buf + 8, h * 2u); // XOR + AND
  ico_write_u16(buf + 12, 1u);    // planes
  ico_write_u16(buf + 14, 32u);   // bpp
  ico_write_u32(buf + 16, 0u);    // BI_RGB
  ico_write_u32(buf + 20, (uint32_t)(xor_bytes + mask_bytes));

  const uint8_t * src = (const uint8_t *)gimg_raster_pixels_const(raster);
  size_t src_stride = gimg_raster_stride_bytes(raster);
  unsigned char * xor_base = buf + 40u;
  for (uint32_t y = 0; y < h; y++) {
    // Bottom-up
    const uint8_t * srow = src + (size_t)(h - 1u - y) * src_stride;
    unsigned char * drow = xor_base + (size_t)y * xor_stride;
    for (uint32_t x = 0; x < w; x++) {
      uint8_t rgba[4];
      ico_sample(fmt, srow, x, rgba);
      drow[x * 4u + 0u] = rgba[2]; // B
      drow[x * 4u + 1u] = rgba[1]; // G
      drow[x * 4u + 2u] = rgba[0]; // R
      drow[x * 4u + 3u] = rgba[3]; // A
    }
  }
  // AND mask: transparent where alpha == 0
  unsigned char * mask_base = xor_base + xor_bytes;
  for (uint32_t y = 0; y < h; y++) {
    const uint8_t * srow = src + (size_t)(h - 1u - y) * src_stride;
    unsigned char * mrow = mask_base + (size_t)y * mask_stride;
    for (uint32_t x = 0; x < w; x++) {
      uint8_t rgba[4];
      ico_sample(fmt, srow, x, rgba);
      if (rgba[3] == 0u) {
        mrow[x / 8u] |= (unsigned char)(0x80u >> (x % 8u));
      }
    }
  }

  *out_bytes = buf;
  *out_size = total;
  return GIMG_OK;
}

static GIMG_Result ico_encode_png(const GIMG_Allocator * alloc,
    const GIMG_Raster * raster, unsigned char ** out_bytes, size_t * out_size) {
  GIMG_Codec * png = gimg_codec_by_name("png");
  if (!png || !png->save_cb) {
    return GIMG_ERR_UNSUPPORTED;
  }
  GIMG_Doc * doc = NULL;
  GIMG_Result r = gimg_doc_from_raster_with_allocator(alloc, raster, &doc);
  if (r != GIMG_OK) {
    return r;
  }
  GIMG_Stream * stream = NULL;
  r = gimg_stream_create_memory_output_with_allocator(alloc, &stream);
  if (r != GIMG_OK) {
    gimg_doc_destroy(doc);
    return r;
  }
  GIMG_Save_Report report = {0};
  r = png->save_cb(png, doc, stream, "png", NULL, &report);
  gimg_doc_destroy(doc);
  if (r != GIMG_OK) {
    gimg_stream_destroy(stream);
    return r;
  }
  const void * data = NULL;
  size_t n = 0;
  gimg_stream_output_buffer(stream, &data, &n);
  if (!data || n == 0u) {
    gimg_stream_destroy(stream);
    return GIMG_ERR_INTERNAL;
  }
  unsigned char * copy = (unsigned char *)gimg_malloc(alloc, n);
  if (!copy) {
    gimg_stream_destroy(stream);
    return GIMG_ERR_OOM;
  }
  memcpy(copy, data, n);
  gimg_stream_destroy(stream);
  *out_bytes = copy;
  *out_size = n;
  return GIMG_OK;
}

GIMG_Result gimg_ico_save(GIMG_Codec * codec, const GIMG_Doc * doc,
    GIMG_Stream * stream, const char * format_name,
    const GIMG_Save_Options * options, GIMG_Save_Report * report) {
  (void)format_name;
  if (!codec || !doc || !stream) {
    return GIMG_ERR_INTERNAL;
  }
  if (report) {
    memset(report, 0, sizeof(*report));
  }
  if (doc->loaded_by_codec && doc->loaded_by_codec != codec) {
    // Foreign codec_private must not be touched; we only need items/rasters.
  }

  const GIMG_Allocator * alloc = gimg_alloc_or_default(codec->allocator);
  uint8_t payload_opt =
      options ? options->ico_payload : (uint8_t)GIMG_ICO_PAYLOAD_AUTO;

  size_t n = gimg_doc_item_count(doc);
  if (n == 0u || n > GIMG_ICO_MAX_ENTRIES) {
    return GIMG_ERR_UNSUPPORTED;
  }

  // Collect IMAGE + ALTERNATE; refuse FRAME.
  size_t * indices = (size_t *)gimg_malloc(alloc, n * sizeof(size_t));
  if (!indices) {
    return GIMG_ERR_OOM;
  }
  size_t count = 0;
  int any_hotspot = 0;
  for (size_t i = 0; i < n; i++) {
    GIMG_Item * item = gimg_doc_item(doc, i);
    GIMG_Item_Role role = gimg_item_role(item);
    if (role == GIMG_ITEM_FRAME) {
      gimg_free(alloc, indices);
      return GIMG_ERR_UNSUPPORTED;
    }
    if (role != GIMG_ITEM_IMAGE && role != GIMG_ITEM_ALTERNATE) {
      continue;
    }
    uint16_t hx = 0, hy = 0;
    gimg_item_hotspot(item, &hx, &hy);
    if (hx || hy) {
      any_hotspot = 1;
    }
    indices[count++] = i;
  }
  if (count == 0u) {
    gimg_free(alloc, indices);
    return GIMG_ERR_UNSUPPORTED;
  }

  typedef struct {
    unsigned char * bytes;
    size_t size;
    uint32_t width;
    uint32_t height;
    uint16_t hotspot_x;
    uint16_t hotspot_y;
    int is_png;
  } payload_t;

  payload_t * payloads =
      (payload_t *)gimg_calloc(alloc, count, sizeof(payload_t));
  if (!payloads) {
    gimg_free(alloc, indices);
    return GIMG_ERR_OOM;
  }

  GIMG_Result r = GIMG_OK;
  for (size_t i = 0; i < count; i++) {
    GIMG_Item * item = gimg_doc_item(doc, indices[i]);
    GIMG_Raster * raster = NULL;
    int owned = 0;
    if (gimg_item_raster(item)) {
      raster = gimg_item_raster(item);
    }
    else {
      r = gimg_item_decode(item, NULL, &raster);
      owned = 1;
      if (r != GIMG_OK) {
        break;
      }
    }
    // Ensure RGBA8 for encoding.
    if (!raster) {
      r = GIMG_ERR_UNSUPPORTED;
      break;
    }
    payloads[i].width = gimg_raster_width(raster);
    payloads[i].height = gimg_raster_height(raster);
    gimg_item_hotspot(item, &payloads[i].hotspot_x, &payloads[i].hotspot_y);
    payloads[i].is_png = ico_choose_png(raster, payload_opt);
    if (payloads[i].is_png) {
      r = ico_encode_png(
          alloc, raster, &payloads[i].bytes, &payloads[i].size);
    }
    else {
      r = ico_encode_dib(
          alloc, raster, &payloads[i].bytes, &payloads[i].size);
    }
    if (owned) {
      gimg_raster_destroy(raster);
    }
    if (r != GIMG_OK) {
      break;
    }
  }

  if (r == GIMG_OK) {
    uint16_t type =
        any_hotspot ? (uint16_t)GIMG_ICO_TYPE_CURSOR : (uint16_t)GIMG_ICO_TYPE_ICON;
    size_t dir_end =
        GIMG_ICO_DIR_SIZE + count * GIMG_ICO_DIRENTRY_SIZE;
    size_t total = dir_end;
    for (size_t i = 0; i < count; i++) {
      total += payloads[i].size;
    }
    unsigned char * out = (unsigned char *)gimg_malloc(alloc, total);
    if (!out) {
      r = GIMG_ERR_OOM;
    }
    else {
      memset(out, 0, total);
      ico_write_u16(out + 0, 0u);
      ico_write_u16(out + 2, type);
      ico_write_u16(out + 4, (uint16_t)count);
      size_t offset = dir_end;
      for (size_t i = 0; i < count; i++) {
        unsigned char * e = out + GIMG_ICO_DIR_SIZE + i * GIMG_ICO_DIRENTRY_SIZE;
        uint32_t dw = payloads[i].width;
        uint32_t dh = payloads[i].height;
        e[0] = (unsigned char)(dw >= 256u ? 0u : dw);
        e[1] = (unsigned char)(dh >= 256u ? 0u : dh);
        e[2] = 0;
        e[3] = 0;
        if (type == GIMG_ICO_TYPE_CURSOR) {
          ico_write_u16(e + 4, payloads[i].hotspot_x);
          ico_write_u16(e + 6, payloads[i].hotspot_y);
        }
        else {
          ico_write_u16(e + 4, 1u); // planes
          ico_write_u16(e + 6, payloads[i].is_png ? 32u : 32u);
        }
        ico_write_u32(e + 8, (uint32_t)payloads[i].size);
        ico_write_u32(e + 12, (uint32_t)offset);
        memcpy(out + offset, payloads[i].bytes, payloads[i].size);
        offset += payloads[i].size;
      }
      size_t written = 0;
      r = gimg_stream_write(stream, out, total, &written);
      if (r == GIMG_OK && written != total) {
        r = GIMG_ERR_IO;
      }
      if (report && r == GIMG_OK) {
        report->bytes_written = written;
      }
      gimg_free(alloc, out);
    }
  }

  for (size_t i = 0; i < count; i++) {
    gimg_free(alloc, payloads[i].bytes);
  }
  gimg_free(alloc, payloads);
  gimg_free(alloc, indices);
  return r;
}

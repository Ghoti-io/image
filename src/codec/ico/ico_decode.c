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
 * ICO/CUR decode: PNG payloads via nested PNG load; DIB via gimg_bmp_load_dib
 * plus AND-mask alpha.
 */

#include <ghoti.io/image/macros.h>
#include <string.h>

#include "../../container/doc_internal.h"
#include "../../core/alloc_internal.h"
#include "../bmp/bmp_internal.h"
#include "../codec_internal.h"
#include "ico_internal.h"

static uint32_t ico_u32(const unsigned char * p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
      ((uint32_t)p[3] << 24);
}

static int32_t ico_i32(const unsigned char * p) {
  uint32_t raw = ico_u32(p);
  if (raw & 0x80000000u) {
    return (int32_t)(raw - 0x100000000ull);
  }
  return (int32_t)raw;
}

static GIMG_Result ico_decode_png(const unsigned char * bytes, size_t size,
    const GIMG_Decode_Options * options, GIMG_Raster ** out_raster) {
  GIMG_Codec * png = gimg_codec_by_name("png");
  if (!png || !png->load_cb || !png->decode_cb) {
    return GIMG_ERR_UNSUPPORTED;
  }
  GIMG_Stream * stream = NULL;
  GIMG_Result r = gimg_stream_create_memory(bytes, size, &stream);
  if (r != GIMG_OK) {
    return r;
  }
  GIMG_Doc * doc = NULL;
  r = png->load_cb(png, stream, NULL, NULL, &doc);
  gimg_stream_destroy(stream);
  if (r != GIMG_OK || !doc || gimg_doc_item_count(doc) == 0) {
    if (doc) {
      gimg_doc_destroy(doc);
    }
    return r != GIMG_OK ? r : GIMG_ERR_CORRUPT;
  }
  r = png->decode_cb(png, gimg_doc_item(doc, 0), options, out_raster);
  gimg_doc_destroy(doc);
  return r;
}

static GIMG_Result ico_decode_dib(const gimg_ico_entry_t * entry,
    const unsigned char * bytes, size_t size, GIMG_Codec * bmp_codec,
    const GIMG_Decode_Options * options, GIMG_Raster ** out_raster) {
  if (size < 40u) {
    return GIMG_ERR_CORRUPT;
  }
  uint32_t header_size = ico_u32(bytes);
  if (header_size < 40u || header_size > size) {
    return GIMG_ERR_CORRUPT;
  }
  int32_t bi_height = ico_i32(bytes + 8);
  uint32_t abs_height =
      bi_height < 0 ? (uint32_t)(-bi_height) : (uint32_t)bi_height;
  // ICO DIB height is XOR + AND; real height is half (when even and > 0).
  uint32_t real_height = abs_height;
  if (abs_height >= 2u && (abs_height % 2u) == 0u) {
    real_height = abs_height / 2u;
  }
  // Prefer the directory when it names a plausible height that matches half.
  uint32_t dir_h = gimg_ico_dir_dim(entry->dir_height);
  if (dir_h != 0u && dir_h == real_height) {
    // already agreed
  }
  else if (dir_h != 0u && dir_h * 2u == abs_height) {
    real_height = dir_h;
  }

  uint32_t compression = ico_u32(bytes + 16);
  // BI_RLE8=1, BI_RLE4=2 — refused for icon payloads.
  if (compression == 1u || compression == 2u) {
    return GIMG_ERR_UNSUPPORTED;
  }

  GIMG_Stream * stream = NULL;
  GIMG_Result r = gimg_stream_create_memory(bytes, size, &stream);
  if (r != GIMG_OK) {
    return r;
  }
  GIMG_Doc * dib_doc = NULL;
  r = gimg_bmp_load_dib(
      bmp_codec, stream, real_height, NULL, NULL, &dib_doc);
  size_t after_xor = gimg_stream_tell(stream);
  if (r != GIMG_OK || !dib_doc) {
    gimg_stream_destroy(stream);
    if (dib_doc) {
      gimg_doc_destroy(dib_doc);
    }
    return r;
  }

  gimg_bmp_doc_state_t * bmp_state =
      (gimg_bmp_doc_state_t *)dib_doc->codec_private;
  if (bmp_state && gimg_bmp_is_rle(bmp_state->header.compression)) {
    gimg_doc_destroy(dib_doc);
    gimg_stream_destroy(stream);
    return GIMG_ERR_UNSUPPORTED;
  }
  uint16_t bit_count = bmp_state ? bmp_state->header.bit_count : 0u;

  GIMG_Raster * raster = NULL;
  r = bmp_codec->decode_cb
      ? bmp_codec->decode_cb(bmp_codec, gimg_doc_item(dib_doc, 0), options,
            &raster)
      : GIMG_ERR_UNSUPPORTED;
  gimg_doc_destroy(dib_doc);
  dib_doc = NULL;
  bmp_state = NULL;
  if (r != GIMG_OK || !raster) {
    gimg_stream_destroy(stream);
    return r != GIMG_OK ? r : GIMG_ERR_CORRUPT;
  }

  // AND mask follows the XOR bitmap in the entry.
  size_t mask_avail = after_xor < size ? size - after_xor : 0u;

  // A 32-bpp ICO DIB stores alpha in the fourth byte even under BI_RGB, which
  // the BMP codec ignores by default (every desktop BMP reader does). Restore
  // those bytes so the identically-zero alpha fallback in §5.3 can see them.
  if (bit_count == 32u && raster) {
    uint32_t w = gimg_raster_width(raster);
    uint32_t h = gimg_raster_height(raster);
    size_t xor_stride = ((size_t)w * 4u + 3u) & ~(size_t)3u;
    size_t xor_bytes = xor_stride * (size_t)h;
    // XOR starts after the DIB header and any palette; after_xor is past it.
    if (after_xor >= xor_bytes) {
      const unsigned char * xor_base = bytes + (after_xor - xor_bytes);
      uint8_t * dest = (uint8_t *)gimg_raster_pixels(raster);
      size_t dest_stride = gimg_raster_stride_bytes(raster);
      for (uint32_t y = 0; y < h; y++) {
        // XOR is bottom-up; raster is top-down.
        const unsigned char * srow =
            xor_base + (size_t)(h - 1u - y) * xor_stride;
        uint8_t * drow = dest + (size_t)y * dest_stride;
        for (uint32_t x = 0; x < w; x++) {
          drow[x * 4u + 3u] = srow[x * 4u + 3u];
        }
      }
    }
  }

  if (mask_avail > 0u) {
    // 1/4/8/16/24 bpp: AND mask is the transparency. 32 bpp: apply only when
    // the alpha channel is identically zero (§5.3).
    int force = (bit_count != 32);
    r = gimg_ico_apply_and_mask(
        raster, bytes + after_xor, mask_avail, force);
    if (r != GIMG_OK) {
      gimg_raster_destroy(raster);
      gimg_stream_destroy(stream);
      return r;
    }
  }
  gimg_stream_destroy(stream);
  *out_raster = raster;
  return GIMG_OK;
}

GIMG_Result gimg_ico_decode(GIMG_Codec * codec, const GIMG_Item * item,
    const GIMG_Decode_Options * options, GIMG_Raster ** out_raster) {
  if (!codec || !item || !out_raster || !item->doc) {
    return GIMG_ERR_INTERNAL;
  }
  *out_raster = NULL;

  GIMG_Doc * doc = item->doc;
  if (doc->loaded_by_codec != codec || !doc->codec_private) {
    return GIMG_ERR_INTERNAL;
  }
  gimg_ico_doc_state_t * state = (gimg_ico_doc_state_t *)doc->codec_private;
  size_t index = item->index;
  if (index >= state->entry_count) {
    return GIMG_ERR_INTERNAL;
  }
  const gimg_ico_entry_t * entry = &state->entries[index];
  if ((size_t)entry->offset + (size_t)entry->size > state->file_size) {
    return GIMG_ERR_CORRUPT;
  }
  const unsigned char * payload = state->file_bytes + entry->offset;

  if (entry->kind == GIMG_ICO_PAYLOAD_KIND_PNG) {
    return ico_decode_png(payload, entry->size, options, out_raster);
  }

  GIMG_Codec * bmp = gimg_codec_by_name("bmp");
  if (!bmp) {
    return GIMG_ERR_UNSUPPORTED;
  }
  GIMG_Result r =
      ico_decode_dib(entry, payload, entry->size, bmp, options, out_raster);
  if (r != GIMG_OK) {
    return r;
  }

  // Directory vs payload disagreement: note it, trust the payload.
  uint32_t dir_w = gimg_ico_dir_dim(entry->dir_width);
  uint32_t dir_h = gimg_ico_dir_dim(entry->dir_height);
  if (*out_raster &&
      (gimg_raster_width(*out_raster) != dir_w ||
          gimg_raster_height(*out_raster) != dir_h)) {
    // Decode has no diagnostics parameter; the note is recorded at load when
    // we can. A mismatch discovered only at decode is still correct to trust
    // the payload — leave the raster as-is.
  }
  return GIMG_OK;
}

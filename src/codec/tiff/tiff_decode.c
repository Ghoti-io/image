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
 * Assemble a TIFF image out of its strips or tiles (TIFF 6.0 sections 3, 15).
 *
 * **A tile is never visible through the API.** Strips and tiles are a storage
 * layout - one picture cut into pieces - in the same way an Adam7 PNG, an
 * interlaced GIF and a non-interleaved JPEG scan are, and this library
 * already undoes all three inside the decoder. A caller gets one whole
 * raster, and the only difference between a stripped file and a tiled one is
 * the shape of the loop below.
 *
 * That is also why the two cases share one pair of arrays: a strip is a tile
 * as wide as the image, so the block loop computes a rectangle either way and
 * copies the part of it that lies inside the picture. A tile's stored data is
 * always the full tile size even at the right and bottom edges, where part of
 * it falls outside; the padding is read and dropped rather than assumed
 * absent.
 */

#include <ghoti.io/image/macros.h>

#include <ghoti.io/cutil/safemath.h>
#include <ghoti.io/image/codec.h>
#include <ghoti.io/image/doc.h>
#include <ghoti.io/image/raster.h>
#include <string.h>

#include "../../container/doc_internal.h"
#include "../../core/alloc_internal.h"
#include "../../core/safe_math_internal.h"
#include "../codec_internal.h"
#include "tiff_internal.h"

/** The rectangle one block covers, before clipping to the image. */
typedef struct {
  uint32_t x, y, width, height;
  size_t row_bytes; ///< Bytes per stored row of the block, padding included.
} tiff_block_t;

static void tiff_block_rect(
    const gimg_tiff_ifd_t * ifd, size_t index, tiff_block_t * out) {
  const size_t spp = ifd->samples_per_pixel;
  if (ifd->tiled) {
    const size_t across =
        ((size_t)ifd->width + ifd->tile_width - 1u) / ifd->tile_width;
    out->x = (uint32_t)((index % across) * ifd->tile_width);
    out->y = (uint32_t)((index / across) * ifd->tile_height);
    out->width = ifd->tile_width;
    out->height = ifd->tile_height;
    out->row_bytes = (size_t)ifd->tile_width * spp;
    return;
  }
  out->x = 0u;
  out->y = (uint32_t)(index * ifd->rows_per_strip);
  out->width = ifd->width;
  out->height = ifd->rows_per_strip;
  out->row_bytes = (size_t)ifd->width * spp;
}

/** One 16-bit ColorMap entry as an 8-bit sample, rounded the way PNG 13.12
 * states and this library converts everywhere else. */
static uint8_t tiff_map8(uint16_t v) {
  return (uint8_t)(((uint32_t)v * 255u + 32767u) / 65535u);
}

/**
 * Copy one row of a block into the raster.
 *
 * @param src First sample of the row inside the block.
 * @param dst First byte of the destination row, at the block's x offset.
 * @param pixels How many pixels of this row lie inside the image.
 */
static void tiff_convert_row(const gimg_tiff_ifd_t * ifd,
    const unsigned char * src, unsigned char * dst, size_t pixels) {
  switch (ifd->photometric) {
  case GIMG_TIFF_PHOTOMETRIC_WHITE_IS_ZERO:
    // Zero is white, so the sample is the complement of the intensity the
    // library's GRAY8 carries (TIFF 6.0 section 8).
    for (size_t i = 0; i < pixels; i++) {
      dst[i] = (unsigned char)(255u - src[i]);
    }
    return;
  case GIMG_TIFF_PHOTOMETRIC_BLACK_IS_ZERO:
    memcpy(dst, src, pixels);
    return;
  case GIMG_TIFF_PHOTOMETRIC_PALETTE: {
    // The map is all reds, then all greens, then all blues (section 8), so
    // each channel is one third of the way further in.
    const size_t third = ifd->color_map_count / 3u;
    for (size_t i = 0; i < pixels; i++) {
      const size_t idx = src[i];
      unsigned char * p = dst + (i * 4u);
      if (idx >= third) {
        // An index the map does not reach.  Opaque black rather than a read
        // past the end; the load already refused a map shorter than the bit
        // depth needs, so this is reachable only from a map longer than three
        // times its third, which no writer produces.
        p[0] = p[1] = p[2] = 0u;
      }
      else {
        p[0] = tiff_map8(ifd->color_map[idx]);
        p[1] = tiff_map8(ifd->color_map[third + idx]);
        p[2] = tiff_map8(ifd->color_map[(third * 2u) + idx]);
      }
      p[3] = 255u;
    }
    return;
  }
  case GIMG_TIFF_PHOTOMETRIC_RGB:
  default: {
    const size_t spp = ifd->samples_per_pixel;
    const bool associated = ifd->has_extra_samples && spp == 4u &&
        ifd->extra_samples == GIMG_TIFF_EXTRA_ASSOCIATED_ALPHA;
    for (size_t i = 0; i < pixels; i++) {
      const unsigned char * s = src + (i * spp);
      unsigned char * p = dst + (i * 4u);
      const unsigned char a = spp == 4u ? s[3] : 255u;
      if (associated && a != 0u && a != 255u) {
        // Associated alpha is premultiplied (section 18) and this library's
        // RGBA8 is not, so the colour is divided back out.  Rounded, and
        // clamped because a file may store a colour brighter than its own
        // alpha allows, which unpremultiplying would otherwise overflow.
        for (int k = 0; k < 3; k++) {
          const unsigned v = ((unsigned)s[k] * 255u + (a / 2u)) / a;
          p[k] = (unsigned char)(v > 255u ? 255u : v);
        }
      }
      else if (associated && a == 0u) {
        p[0] = p[1] = p[2] = 0u;
      }
      else {
        p[0] = s[0];
        p[1] = s[1];
        p[2] = s[2];
      }
      p[3] = a;
    }
    return;
  }
  }
}

GIMG_Result gimg_tiff_decode(GIMG_Codec * codec, const GIMG_Item * item,
    const GIMG_Decode_Options * options, GIMG_Raster ** out_raster) {
  if (!codec || !item || !out_raster) {
    return GIMG_ERR_INTERNAL;
  }
  *out_raster = NULL;
  const GIMG_Doc * doc = item->doc;
  if (!doc || !doc->codec_private) {
    return GIMG_ERR_UNSUPPORTED;
  }
  const gimg_tiff_doc_state_t * st =
      (const gimg_tiff_doc_state_t *)doc->codec_private;
  if (item->index >= st->ifd_count) {
    return GIMG_ERR_INTERNAL;
  }
  const gimg_tiff_ifd_t * ifd = &st->ifds[item->index];
  const GIMG_Allocator * alloc = gimg_alloc_or_default(codec->allocator);
  const GIMG_Limits * limits = options ? options->limits : NULL;

  size_t pixels = 0;
  if (gimg_safe_pixel_count(ifd->width, ifd->height, &pixels) != GIMG_OK) {
    return GIMG_ERR_LIMIT;
  }
  if (limits && limits->max_decoded_pixels &&
      pixels > limits->max_decoded_pixels) {
    return GIMG_ERR_LIMIT;
  }

  // Grayscale keeps one channel; everything else becomes RGBA8, which is what
  // the BMP and GIF decoders also hand back for an indexed or colour image.
  const bool gray = ifd->photometric == GIMG_TIFF_PHOTOMETRIC_WHITE_IS_ZERO ||
      ifd->photometric == GIMG_TIFF_PHOTOMETRIC_BLACK_IS_ZERO;
  const GIMG_Pixel_Format * format = gray ? &GIMG_PIXEL_GRAY8
                                          : &GIMG_PIXEL_RGBA8;
  const size_t out_bpp = gray ? 1u : 4u;

  GIMG_Raster * raster = NULL;
  GIMG_Result r = gimg_raster_create_with_allocator(alloc, ifd->width,
      ifd->height, format, GIMG_RASTER_OWNED, NULL, 0, &raster);
  if (r != GIMG_OK) {
    return r;
  }
  unsigned char * out = (unsigned char *)gimg_raster_pixels(raster);
  const size_t out_stride = gimg_raster_stride_bytes(raster);

  for (size_t b = 0; b < ifd->block_count; b++) {
    tiff_block_t rect;
    tiff_block_rect(ifd, b, &rect);
    if (rect.x >= ifd->width || rect.y >= ifd->height) {
      continue; // A block wholly outside the picture contributes nothing.
    }
    const size_t across =
        (size_t)ifd->width - rect.x < rect.width ? (size_t)ifd->width - rect.x
                                                 : rect.width;
    const size_t down =
        (size_t)ifd->height - rect.y < rect.height
        ? (size_t)ifd->height - rect.y
        : rect.height;
    const unsigned char * src = st->file + ifd->block_offsets[b];
    const size_t have = (size_t)ifd->block_byte_counts[b];

    for (size_t row = 0; row < down; row++) {
      size_t at = 0;
      if (!gcu_safe_mul_size(row, rect.row_bytes, &at)) {
        break;
      }
      // A block shorter than its geometry says is truncated data, not a
      // reason to refuse the picture: the rows that arrived are kept and the
      // rest stay as the raster was created, which is zero.
      if (at + rect.row_bytes > have) {
        break;
      }
      unsigned char * dst = out + ((size_t)(rect.y + row) * out_stride) +
          ((size_t)rect.x * out_bpp);
      tiff_convert_row(ifd, src + at, dst, across);
    }
  }

  *out_raster = raster;
  return GIMG_OK;
}

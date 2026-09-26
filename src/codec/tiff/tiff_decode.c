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

/** Bytes one stored row of @p pixels pixels occupies.
 *
 * Rounded up to a whole byte, because TIFF 6.0 section 3 pads every row to a
 * byte boundary - which is why a 73-pixel-wide 4-bit image has 37-byte rows
 * and not 36.5. Getting this wrong shears the picture one pixel further left
 * on every row, which looks like a decoder that cannot count rather than one
 * that cannot round. */
static size_t tiff_row_bytes(const gimg_tiff_ifd_t * ifd, size_t pixels) {
  const size_t bits = pixels * ifd->samples_per_pixel * ifd->bits_per_sample;
  return (bits + 7u) / 8u;
}

static void tiff_block_rect(
    const gimg_tiff_ifd_t * ifd, size_t index, tiff_block_t * out) {
  if (ifd->tiled) {
    const size_t across =
        ((size_t)ifd->width + ifd->tile_width - 1u) / ifd->tile_width;
    out->x = (uint32_t)((index % across) * ifd->tile_width);
    out->y = (uint32_t)((index / across) * ifd->tile_height);
    out->width = ifd->tile_width;
    out->height = ifd->tile_height;
    out->row_bytes = tiff_row_bytes(ifd, ifd->tile_width);
    return;
  }
  out->x = 0u;
  out->y = (uint32_t)(index * ifd->rows_per_strip);
  out->width = ifd->width;
  out->height = ifd->rows_per_strip;
  out->row_bytes = tiff_row_bytes(ifd, ifd->width);
}

/**
 * One sample out of a packed row.
 *
 * TIFF packs sub-byte samples **most significant bit first** within each
 * byte (section 3), and a row starts on a byte boundary, so sample n of a
 * 4-bit row is the high nibble of byte n/2 when n is even. Sixteen-bit
 * samples are two bytes in the file's own order, which is why this needs to
 * know it: a 16-bit TIFF written on a big-endian machine and read as
 * little-endian is not subtly wrong, it is noise.
 */
static uint32_t tiff_sample(const unsigned char * row, size_t index,
    unsigned bits, bool big_endian) {
  switch (bits) {
  case 8u:
    return row[index];
  case 16u: {
    const unsigned char * p = row + (index * 2u);
    return big_endian ? (((uint32_t)p[0] << 8) | p[1])
                      : (((uint32_t)p[1] << 8) | p[0]);
  }
  default: {
    const size_t bit = index * bits;
    const unsigned shift = (unsigned)(8u - bits - (bit & 7u));
    const uint32_t mask = (1u << bits) - 1u;
    return (row[bit >> 3] >> shift) & mask;
  }
  }
}

/** The largest value a sample of @p bits bits can hold. */
static uint32_t tiff_sample_max(unsigned bits) {
  return (bits >= 32u) ? 0xFFFFFFFFu : ((1u << bits) - 1u);
}

/**
 * A sample of @p bits bits as an 8-bit one.
 *
 * For 1, 2 and 4 bits this is exact and there is nothing to choose: 255 is
 * divisible by 1, 3 and 15, so `v * 255 / max` has no remainder and every
 * reader agrees. Sixteen bits is the case with a choice in it, and this
 * takes the high byte for the reason tiff_map8 does - measured against
 * libtiff rather than assumed.
 */
static uint8_t tiff_to_8(uint32_t v, unsigned bits) {
  if (bits == 8u) {
    return (uint8_t)v;
  }
  if (bits == 16u) {
    return (uint8_t)(v >> 8);
  }
  return (uint8_t)((v * 255u) / tiff_sample_max(bits));
}

/**
 * One ColorMap entry as an 8-bit sample.
 *
 * **The high byte, and not the correctly rounded rescale this library uses
 * everywhere else.** `round(v * 255 / 65535)` is the more accurate of the
 * two and is what `gimg_ops_convert_bit_depth` and the BMP decoder do; it is
 * not what TIFF readers do. libtiff narrows a ColorMap with `v >> 8`, which
 * is `floor(v / 256)` - a slightly different scale that lands on 255 for
 * 65535 by luck of the truncation - and the two disagree by one on about
 * half the entries of a real map.
 *
 * Measured rather than assumed: over every pixel of the libtiff sample set's
 * `depth/flower-palette-08.tif`, `v >> 8` matches libtiff 3139 times out of
 * 3139, the rounded rescale 2709 and the truncated rescale 1857.
 *
 * Matching is the right call for the same reason the eight-bit-map guess
 * below it is. A TIFF ColorMap means in practice what TIFF readers make of
 * it, there being no independent conformance suite that says otherwise, and
 * half a palette that disagrees with every other reader by one is worse than
 * a scale that is 0.4% out. Being half-libtiff and half-PNG here would be the
 * worst of both.
 *
 * A map the loader judged eight-bit - every entry below 256, which is what
 * most writers produce - is taken at face value; tiff_load.c carries that
 * argument, and libtiff's `checkcmap` makes the same test.
 */
static uint8_t tiff_map8(const gimg_tiff_ifd_t * ifd, uint16_t v) {
  if (ifd->color_map_is_8bit) {
    return (uint8_t)v;
  }
  return (uint8_t)(v >> 8);
}

/**
 * Copy one row of a block into the raster.
 *
 * @param src First sample of the row inside the block.
 * @param dst First byte of the destination row, at the block's x offset.
 * @param pixels How many pixels of this row lie inside the image.
 *
 * The destination is 16-bit when @p wide, and then every value written is a
 * 16-bit one in host order - which is what GIMG_PIXEL_GRAY16 and RGBA16 mean.
 */
static void tiff_convert_row(const gimg_tiff_ifd_t * ifd,
    const unsigned char * src, unsigned char * dst, size_t pixels,
    bool wide) {
  const unsigned bits = ifd->bits_per_sample;
  const bool be = ifd->file_big_endian;
  const size_t spp = ifd->samples_per_pixel;
  uint16_t * dst16 = (uint16_t *)(void *)dst;

  switch (ifd->photometric) {
  case GIMG_TIFF_PHOTOMETRIC_WHITE_IS_ZERO:
  case GIMG_TIFF_PHOTOMETRIC_BLACK_IS_ZERO: {
    // Zero is white in one of these and black in the other (section 8), and
    // the complement is taken in whatever width the output is, not in the
    // file's - complementing a 4-bit sample and then widening it is a
    // different picture from widening it and then complementing.
    const bool invert =
        ifd->photometric == GIMG_TIFF_PHOTOMETRIC_WHITE_IS_ZERO;
    for (size_t i = 0; i < pixels; i++) {
      const uint32_t v = tiff_sample(src, i * spp, bits, be);
      if (wide) {
        const uint16_t w16 = (uint16_t)v;
        dst16[i] = invert ? (uint16_t)(65535u - w16) : w16;
      }
      else {
        const uint8_t v8 = tiff_to_8(v, bits);
        dst[i] = invert ? (uint8_t)(255u - v8) : v8;
      }
    }
    return;
  }
  case GIMG_TIFF_PHOTOMETRIC_PALETTE: {
    // The map is all reds, then all greens, then all blues (section 8), so
    // each channel is one third of the way further in.
    const size_t third = ifd->color_map_count / 3u;
    for (size_t i = 0; i < pixels; i++) {
      const size_t idx = tiff_sample(src, i, bits, be);
      unsigned char * p = dst + (i * 4u);
      if (idx >= third) {
        // An index the map does not reach. Opaque black rather than a read
        // past the end; the load already refused a map shorter than the bit
        // depth needs, so this is reachable only from a map longer than three
        // times its third, which no writer produces.
        p[0] = p[1] = p[2] = 0u;
      }
      else {
        p[0] = tiff_map8(ifd, ifd->color_map[idx]);
        p[1] = tiff_map8(ifd, ifd->color_map[third + idx]);
        p[2] = tiff_map8(ifd, ifd->color_map[(third * 2u) + idx]);
      }
      p[3] = 255u;
    }
    return;
  }
  case GIMG_TIFF_PHOTOMETRIC_RGB:
  default: {
    const bool associated = ifd->has_extra_samples && spp >= 4u &&
        ifd->extra_samples == GIMG_TIFF_EXTRA_ASSOCIATED_ALPHA;
    const uint32_t full = wide ? 65535u : 255u;
    for (size_t i = 0; i < pixels; i++) {
      const size_t at = i * spp;
      uint32_t ch[4];
      for (size_t k = 0; k < 3u; k++) {
        const uint32_t v = tiff_sample(src, at + k, bits, be);
        ch[k] = wide ? v : tiff_to_8(v, bits);
      }
      ch[3] = (spp >= 4u)
          ? (wide ? tiff_sample(src, at + 3u, bits, be)
                  : tiff_to_8(tiff_sample(src, at + 3u, bits, be), bits))
          : full;
      if (associated && ch[3] != 0u && ch[3] != full) {
        // Associated alpha is premultiplied (section 18) and this library's
        // RGBA is not, so the colour is divided back out. Rounded, and
        // clamped because a file may store a colour brighter than its own
        // alpha allows, which unpremultiplying would otherwise overflow.
        for (size_t k = 0; k < 3u; k++) {
          const uint32_t v =
              ((uint32_t)ch[k] * full + (ch[3] / 2u)) / ch[3];
          ch[k] = v > full ? full : v;
        }
      }
      else if (associated && ch[3] == 0u) {
        ch[0] = ch[1] = ch[2] = 0u;
      }
      if (wide) {
        for (size_t k = 0; k < 4u; k++) {
          dst16[(i * 4u) + k] = (uint16_t)ch[k];
        }
      }
      else {
        for (size_t k = 0; k < 4u; k++) {
          dst[(i * 4u) + k] = (uint8_t)ch[k];
        }
      }
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

  // Grayscale keeps one channel; everything else becomes RGBA, which is what
  // the BMP and GIF decoders also hand back for an indexed or colour image.
  //
  // Sixteen bits stay sixteen bits. Narrowing here would be a decision about
  // the picture rather than about how it is stored, and this library has
  // GRAY16 and RGBA16 precisely so a caller can make that decision itself; a
  // palette image is the exception, because its map is narrowed to eight
  // whatever the indices are wide.
  const bool gray = ifd->photometric == GIMG_TIFF_PHOTOMETRIC_WHITE_IS_ZERO ||
      ifd->photometric == GIMG_TIFF_PHOTOMETRIC_BLACK_IS_ZERO;
  const bool wide = ifd->bits_per_sample == 16u &&
      ifd->photometric != GIMG_TIFF_PHOTOMETRIC_PALETTE;
  const GIMG_Pixel_Format * format = gray
      ? (wide ? &GIMG_PIXEL_GRAY16 : &GIMG_PIXEL_GRAY8)
      : (wide ? &GIMG_PIXEL_RGBA16 : &GIMG_PIXEL_RGBA8);
  const size_t out_bpp = (gray ? 1u : 4u) * (wide ? 2u : 1u);

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
      tiff_convert_row(ifd, src + at, dst, across, wide);
    }
  }

  *out_raster = raster;
  return GIMG_OK;
}

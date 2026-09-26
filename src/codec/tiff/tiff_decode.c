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
#include <ghoti.io/image/color.h>
#include <ghoti.io/image/raster.h>
#include <stdint.h>
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
  int plane;        ///< Which channel, or -1 when the block carries them all.
} tiff_block_t;

/** Bytes one stored row of @p pixels pixels occupies.
 *
 * Rounded up to a whole byte, because TIFF 6.0 section 3 pads every row to a
 * byte boundary - which is why a 73-pixel-wide 4-bit image has 37-byte rows
 * and not 36.5. Getting this wrong shears the picture one pixel further left
 * on every row, which looks like a decoder that cannot count rather than one
 * that cannot round. */
static size_t tiff_row_bytes(
    const gimg_tiff_ifd_t * ifd, size_t pixels, bool planar) {
  const size_t per_pixel = planar ? 1u : ifd->samples_per_pixel;
  const size_t bits = pixels * per_pixel * ifd->bits_per_sample;
  return (bits + 7u) / 8u;
}

/**
 * The rectangle block @p index covers, and which channel it carries.
 *
 * With PlanarConfiguration 2 the file holds one whole set of strips or tiles
 * per sample, laid out plane after plane, so the block index divides into a
 * plane and a position within it. With configuration 1 there is one plane and
 * every block carries every channel, which is spelled here as plane -1.
 */
static void tiff_block_rect(
    const gimg_tiff_ifd_t * ifd, size_t index, tiff_block_t * out) {
  out->plane = -1;
  if (ifd->planar_config == 2u && ifd->blocks_per_plane > 0u) {
    out->plane = (int)(index / ifd->blocks_per_plane);
    index %= ifd->blocks_per_plane;
  }
  if (ifd->tiled) {
    const size_t across =
        ((size_t)ifd->width + ifd->tile_width - 1u) / ifd->tile_width;
    out->x = (uint32_t)((index % across) * ifd->tile_width);
    out->y = (uint32_t)((index / across) * ifd->tile_height);
    out->width = ifd->tile_width;
    out->height = ifd->tile_height;
    out->row_bytes =
        tiff_row_bytes(ifd, ifd->tile_width, out->plane >= 0);
    return;
  }
  out->x = 0u;
  out->y = (uint32_t)(index * ifd->rows_per_strip);
  out->width = ifd->width;
  out->height = ifd->rows_per_strip;
  out->row_bytes = tiff_row_bytes(ifd, ifd->width, out->plane >= 0);
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

/** What the decoded raster looks like, decided once from the directory. */
typedef struct {
  const GIMG_Pixel_Format * format;
  size_t channels;   ///< Channels in the output raster.
  size_t bytes;      ///< Bytes per output pixel.
  bool wide;         ///< 16-bit samples rather than 8.
  bool has_alpha;    ///< The output carries an alpha channel.
  bool source_alpha; ///< ...and the file supplies it.
} tiff_output_t;

static bool tiff_plan_output(
    const gimg_tiff_ifd_t * ifd, tiff_output_t * out) {
  const bool gray = ifd->photometric == GIMG_TIFF_PHOTOMETRIC_WHITE_IS_ZERO ||
      ifd->photometric == GIMG_TIFF_PHOTOMETRIC_BLACK_IS_ZERO;
  const bool cmyk = ifd->photometric == GIMG_TIFF_PHOTOMETRIC_CMYK;
  // Sixteen bits stay sixteen bits: narrowing would be a decision about the
  // picture rather than about how it is stored, and GRAY16, RGBA16 and CMYK16
  // exist so the caller makes it. A palette is the exception, because its map
  // is narrowed on the way into an 8-bit raster whatever the indices are wide.
  out->wide = ifd->bits_per_sample == 16u &&
      ifd->photometric != GIMG_TIFF_PHOTOMETRIC_PALETTE;
  if (gray) {
    out->format = out->wide ? &GIMG_PIXEL_GRAY16 : &GIMG_PIXEL_GRAY8;
    out->channels = 1u;
    out->has_alpha = false;
  }
  else if (cmyk) {
    out->format = out->wide ? &GIMG_PIXEL_CMYK16 : &GIMG_PIXEL_CMYK8;
    out->channels = 4u;
    out->has_alpha = false;
  }
  else {
    out->format = out->wide ? &GIMG_PIXEL_RGBA16 : &GIMG_PIXEL_RGBA8;
    out->channels = 4u;
    out->has_alpha = true;
  }
  out->bytes = out->channels * (out->wide ? 2u : 1u);
  out->source_alpha = out->has_alpha && ifd->samples_per_pixel >= 4u;
  return out->format != NULL;
}

/** Write one value into channel @p k of output pixel @p at. */
static void tiff_put(const tiff_output_t * out, unsigned char * pixel,
    size_t k, uint32_t v) {
  if (out->wide) {
    ((uint16_t *)(void *)pixel)[k] = (uint16_t)v;
  }
  else {
    pixel[k] = (unsigned char)v;
  }
}

/**
 * Copy one row of a block into the raster.
 *
 * @param src First sample of the row inside the block.
 * @param dst First byte of the destination row, at the block's x offset.
 * @param pixels How many pixels of this row lie inside the image.
 * @param plane Which channel this block carries, or -1 for all of them.
 *
 * Alpha is not touched here. A file with associated alpha needs every channel
 * of a pixel in hand to divide it back out, and with PlanarConfiguration 2
 * the channels arrive in separate blocks - so that runs once over the
 * finished raster instead, which is also one implementation of it rather than
 * two that can drift.
 */
static void tiff_convert_row(const gimg_tiff_ifd_t * ifd,
    const tiff_output_t * out, const unsigned char * src, unsigned char * dst,
    size_t pixels, int plane) {
  const unsigned bits = ifd->bits_per_sample;
  const bool be = ifd->file_big_endian;
  const size_t spp = ifd->samples_per_pixel;
  const uint32_t full = out->wide ? 65535u : 255u;

  if (ifd->photometric == GIMG_TIFF_PHOTOMETRIC_PALETTE) {
    // The map is all reds, then all greens, then all blues (section 8), so
    // each channel is one third of the way further in. Always interleaved:
    // a palette image has one sample per pixel, so it has one plane.
    const size_t third = ifd->color_map_count / 3u;
    for (size_t i = 0; i < pixels; i++) {
      const size_t idx = tiff_sample(src, i, bits, be);
      unsigned char * p = dst + (i * out->bytes);
      if (idx >= third) {
        // An index the map does not reach. Opaque black rather than a read
        // past the end; the load already refused a map shorter than the bit
        // depth needs.
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

  // Zero is white in one photometric and black in the other (section 8). The
  // complement is taken in the output's width, not the file's: complementing
  // a 4-bit sample and then widening it is a different picture from widening
  // it and then complementing.
  const bool invert =
      ifd->photometric == GIMG_TIFF_PHOTOMETRIC_WHITE_IS_ZERO;

  for (size_t i = 0; i < pixels; i++) {
    unsigned char * pixel = dst + (i * out->bytes);
    for (size_t k = 0; k < out->channels; k++) {
      if (plane >= 0 && (size_t)plane != k) {
        continue; // Another block carries this channel.
      }
      if (k >= spp) {
        // An output channel the file has no sample for: the alpha of a
        // three-sample RGB image. Filled after the block loop, not here,
        // because with separate planes no block would own it.
        continue;
      }
      const size_t at = (plane >= 0) ? i : (i * spp) + k;
      uint32_t v = tiff_sample(src, at, bits, be);
      if (!out->wide) {
        v = tiff_to_8(v, bits);
      }
      if (invert) {
        v = full - v;
      }
      tiff_put(out, pixel, k, v);
    }
  }
}

/** Fill an alpha channel the file did not supply. */
static void tiff_fill_alpha(const tiff_output_t * out, unsigned char * pixels,
    size_t stride, uint32_t width, uint32_t height) {
  const uint32_t full = out->wide ? 65535u : 255u;
  for (uint32_t y = 0; y < height; y++) {
    unsigned char * row = pixels + ((size_t)y * stride);
    for (uint32_t x = 0; x < width; x++) {
      tiff_put(out, row + ((size_t)x * out->bytes), 3u, full);
    }
  }
}

/**
 * Divide out an associated alpha, over the finished raster.
 *
 * TIFF 6.0 section 18 calls it associated alpha and means premultiplied;
 * this library's RGBA is not, so the colour has to be divided back out. Done
 * here rather than in the row converter because with PlanarConfiguration 2
 * the colour and the alpha arrive in different blocks, and doing it twice -
 * once per layout - is one logic written twice.
 */
static void tiff_unpremultiply(const tiff_output_t * out,
    unsigned char * pixels, size_t stride, uint32_t width, uint32_t height) {
  const uint32_t full = out->wide ? 65535u : 255u;
  for (uint32_t y = 0; y < height; y++) {
    unsigned char * row = pixels + ((size_t)y * stride);
    for (uint32_t x = 0; x < width; x++) {
      unsigned char * p = row + ((size_t)x * out->bytes);
      const uint32_t a = out->wide ? ((uint16_t *)(void *)p)[3] : p[3];
      if (a == full) {
        continue;
      }
      if (a == 0u) {
        // Nothing to divide by, and nothing to recover: a premultiplied
        // colour at zero alpha should be zero, and where a writer's rounding
        // left it at one, one is the most faithful thing to hand back.
        // Zeroing it here would be inventing a value the file did not store.
        continue;
      }
      for (size_t k = 0; k < 3u; k++) {
        const uint32_t v = out->wide ? ((uint16_t *)(void *)p)[k] : p[k];
        // A file may store a colour brighter than its own alpha allows,
        // which the division would otherwise overflow. Saturating is the
        // only thing an unassociated raster of this width can do with it,
        // and it is where the conversion loses information: the excess
        // cannot be recovered by multiplying back.
        const uint32_t scaled = (v * full + (a / 2u)) / a;
        tiff_put(out, p, k, scaled > full ? full : scaled);
      }
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

  tiff_output_t out;
  if (!tiff_plan_output(ifd, &out)) {
    return GIMG_ERR_UNSUPPORTED;
  }

  GIMG_Raster * raster = NULL;
  GIMG_Result r = gimg_raster_create_with_allocator(alloc, ifd->width,
      ifd->height, out.format, GIMG_RASTER_OWNED, NULL, 0, &raster);
  if (r != GIMG_OK) {
    return r;
  }
  unsigned char * dst_pixels = (unsigned char *)gimg_raster_pixels(raster);
  const size_t stride = gimg_raster_stride_bytes(raster);

  for (size_t b = 0; b < ifd->block_count; b++) {
    tiff_block_t rect;
    tiff_block_rect(ifd, b, &rect);
    if (rect.x >= ifd->width || rect.y >= ifd->height) {
      continue; // A block wholly outside the picture contributes nothing.
    }
    if (rect.plane >= 0 && (size_t)rect.plane >= out.channels) {
      continue; // A plane the output has no channel for.
    }
    const size_t across =
        (size_t)ifd->width - rect.x < rect.width ? (size_t)ifd->width - rect.x
                                                 : rect.width;
    const size_t down =
        (size_t)ifd->height - rect.y < rect.height
        ? (size_t)ifd->height - rect.y
        : rect.height;
    // What the block holds once expanded, and it is `down` rows rather than
    // `rect.height` on purpose.
    //
    // A strip cannot hold more rows than the image has left, whatever
    // RowsPerStrip says, and a hostile file says something enormous: the
    // fuzzer's first find here was an 8-row image declaring 536,870,920 rows
    // per strip, which is a consistent-looking file - one strip, one offset,
    // one byte count - that asked this decoder for an 8.6 GB buffer. Sizing
    // the expansion by the picture rather than by the tag is what makes that
    // a short read instead of an allocation.
    size_t want = 0;
    if (!gcu_safe_mul_size(down, rect.row_bytes, &want)) {
      gimg_raster_destroy(raster);
      return GIMG_ERR_LIMIT;
    }
    const unsigned char * src = NULL;
    size_t have = 0;
    bool owned = false;
    r = gimg_tiff_block_bytes(st, ifd, b, want, &src, &have, &owned);
    if (r != GIMG_OK) {
      gimg_raster_destroy(raster);
      return r;
    }
    if (owned) {
      // The predictor is undone over the expanded block, before any row of
      // it is read, because a difference is relative to the sample before it
      // and the row converter works one row at a time.
      gimg_tiff_undo_block_predictor(ifd, (unsigned char *)(uintptr_t)src,
          have, rect.row_bytes,
          rect.plane >= 0 ? 1u : ifd->samples_per_pixel);
    }

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
      unsigned char * dst = dst_pixels +
          ((size_t)(rect.y + row) * stride) + ((size_t)rect.x * out.bytes);
      tiff_convert_row(ifd, &out, src + at, dst, across, rect.plane);
    }
    if (owned) {
      gimg_free(alloc, (void *)(uintptr_t)src);
    }
  }

  // Alpha last, and over the whole raster: a three-sample RGB image has none
  // to read, and an associated one needs every channel of a pixel in hand.
  if (out.has_alpha && !out.source_alpha &&
      ifd->photometric != GIMG_TIFF_PHOTOMETRIC_PALETTE) {
    tiff_fill_alpha(&out, dst_pixels, stride, ifd->width, ifd->height);
  }
  if (out.source_alpha && ifd->has_extra_samples &&
      ifd->extra_samples == GIMG_TIFF_EXTRA_ASSOCIATED_ALPHA) {
    tiff_unpremultiply(&out, dst_pixels, stride, ifd->width, ifd->height);
  }

  // The profile the file carried, applied to the raster it describes rather
  // than to the document: a multi-page TIFF's pages may each have their own,
  // and a profile is a statement about one picture's colours.
  if (ifd->icc && ifd->icc_size > 0u) {
    GIMG_Color_Info info;
    gimg_color_info_default(&info);
    const GIMG_Color_Info * existing = gimg_raster_color_info_const(raster);
    if (existing) {
      info = *existing;
    }
    info.icc_bytes = ifd->icc;
    info.icc_size = ifd->icc_size;
    // set_color_info copies the profile, which is why the IFD may keep
    // owning these bytes and the raster may outlive nothing.
    (void)gimg_raster_set_color_info(raster, &info);
  }

  *out_raster = raster;
  return GIMG_OK;
}

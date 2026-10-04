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
#include <ghoti.io/color/color.h>
#include <ghoti.io/image/raster.h>
#include <math.h>
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
/**
 * One sample out of a packed row.
 *
 * Two layouts, and which one applies is decided by the depth alone.
 *
 * A depth that is a whole number of bytes is stored as those bytes in the
 * file's own order, so sixteen, twenty-four and thirty-two bits are read
 * here and not bit by bit. Anything else is packed **most significant bit
 * first, continuously**: TIFF 6.0 leaves no padding between samples and pads
 * only the row, so a twelve-bit sample straddles a byte boundary every other
 * time and a ten-bit one four times in five.
 *
 * The byte order applies to the first layout and not to the second, which is
 * the part worth stating: a bit-packed row is a bit stream and has no
 * multi-byte words for an order to apply to.
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
  case 24u: {
    const unsigned char * p = row + (index * 3u);
    return big_endian
        ? (((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2])
        : (((uint32_t)p[2] << 16) | ((uint32_t)p[1] << 8) | p[0]);
  }
  case 32u: {
    const unsigned char * p = row + (index * 4u);
    return big_endian ? (((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
                            ((uint32_t)p[2] << 8) | p[3])
                      : (((uint32_t)p[3] << 24) | ((uint32_t)p[2] << 16) |
                            ((uint32_t)p[1] << 8) | p[0]);
  }
  default: {
    // The general bit-packed case. It reads a byte at a time rather than
    // loading a word and shifting, because a sample at the end of a row can
    // start in the row's last byte and a word load would read past it.
    size_t bit = index * bits;
    uint32_t v = 0;
    for (unsigned left = bits; left > 0u;) {
      const unsigned in_byte = 8u - (unsigned)(bit & 7u);
      const unsigned take = left < in_byte ? left : in_byte;
      const unsigned shift = in_byte - take;
      const uint32_t mask = (uint32_t)((1u << take) - 1u);
      v = (v << take) | ((row[bit >> 3] >> shift) & mask);
      bit += take;
      left -= take;
    }
    return v;
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
 * **Only ever called with 1, 2, 4 or 8**, because tiff_plan_output sends
 * every other depth to a 16-bit raster - see the note there for why. That is
 * what makes this exact and leaves nothing to choose: 255 is divisible by 1,
 * 3 and 15, so `v * 255 / max` has no remainder and every reader agrees.
 *
 * It is written as the general map rather than as a table of four so that a
 * depth arriving here later is widened correctly rather than quietly; the
 * rounding term costs nothing and never fires at those four depths.
 */
static uint8_t tiff_to_8(uint32_t v, unsigned bits) {
  if (bits >= 8u) {
    return (uint8_t)(bits == 8u ? v : (v >> (bits - 8u)));
  }
  const uint32_t max = tiff_sample_max(bits);
  return (uint8_t)(((v * 255u) + (max / 2u)) / max);
}

/**
 * A sample of any depth as sixteen bits.
 *
 * Both directions are the same full-range map, `v * 65535 / max`, rather
 * than a shift: a twelve-bit 4095 has to come back as 65535 and not as
 * 65520, or the brightest thing in the picture stops being white.
 *
 * **They round differently, and that is measured rather than chosen.**
 * Against ImageMagick 7.1.1 on the libtiff sample set's depth series, all
 * 3,139 samples of one picture at each depth:
 *
 *   | depth | truncating | rounded |
 *   |---|---|---|
 *   | 10, 12, 14 | 1,587 of 3,139 | **3,139 of 3,139** |
 *   | 24, 32 | **3,139 of 3,139** | 535 of 3,139, off by up to 2 |
 *
 * So widening rounds and narrowing truncates. It reads as an inconsistency
 * and it is one - ImageMagick's - but the alternative to reproducing it is
 * disagreeing with the only reference that reads these depths at all.
 */
static uint16_t tiff_to_16(uint32_t v, unsigned bits) {
  if (bits == 16u) {
    return (uint16_t)v;
  }
  const uint32_t max = tiff_sample_max(bits);
  if (bits > 16u) {
    return (uint16_t)(((uint64_t)v * 65535u) / max);
  }
  return (uint16_t)((((uint64_t)v * 65535u) + (max / 2u)) / max);
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
  // A compression-7 file declares PhotometricInterpretation 6 and means it
  // about the *JPEG's* samples, which the JPEG decoder has already converted
  // by the time this codec sees them. Treating it as YCbCr here would
  // convert a second time.
  const bool ycbcr = ifd->photometric == GIMG_TIFF_PHOTOMETRIC_YCBCR &&
      ifd->compression != GIMG_TIFF_COMPRESSION_JPEG &&
      ifd->compression != GIMG_TIFF_COMPRESSION_JPEG_OLD;
  // Lab samples are not RGB samples stored wide. Both encodings come out as
  // eight-bit RGB, which is the conversion libtiff's RGBA reader performs.
  const bool lab = ifd->photometric == GIMG_TIFF_PHOTOMETRIC_CIELAB ||
      ifd->photometric == GIMG_TIFF_PHOTOMETRIC_ICCLAB;
  // Sixteen bits stay sixteen bits: narrowing would be a decision about the
  // picture rather than about how it is stored, and GRAY16, RGBA16 and CMYK16
  // exist so the caller makes it. A palette is the exception, because its map
  // is narrowed on the way into an 8-bit raster whatever the indices are wide.
  // **Which depths fit in an eight-bit raster, and which do not.**
  //
  // One, two, four and eight bits map onto eight exactly - each level
  // becomes v*255, v*85, v*17 or v - so an eight-bit raster loses nothing
  // and is the friendlier answer. Every other depth does not: six bits has
  // 64 levels and 63 does not divide 255, so an eight-bit raster would
  // quantise a picture that was already stored exactly. Those go to sixteen,
  // where the full-range map is exact.
  //
  // Measured against ImageMagick on the sample set's depth series: at six
  // bits an eight-bit raster disagreed on 3,071 samples of 3,139 and a
  // sixteen-bit one agrees on all of them.
  const unsigned depth = ifd->bits_per_sample;
  const bool fits_in_8 =
      depth == 1u || depth == 2u || depth == 4u || depth == 8u;
  out->wide = !fits_in_8 &&
      ifd->photometric != GIMG_TIFF_PHOTOMETRIC_PALETTE && !ycbcr && !lab;
  if (ycbcr || lab) {
    // Converted to RGB on the way out. YCbCr and Lab are ways of storing
    // colour rather than colour models this library's rasters carry. The
    // JPEG decoder does the same with YCbCr, and Lab follows libtiff's
    // RGBA reader, which lands on eight bits for both depths.
    out->format = &GIMG_PIXEL_RGBA8;
    out->channels = 4u;
    out->has_alpha = true;
  }
  else if (gray) {
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
  out->source_alpha =
      out->has_alpha && !ycbcr && ifd->samples_per_pixel >= 4u;
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
      v = out->wide ? tiff_to_16(v, bits) : tiff_to_8(v, bits);
      if (invert) {
        v = full - v;
      }
      tiff_put(out, pixel, k, v);
    }
  }
}

/**
 * One YCbCr triple as RGB, by the formula section 21 states.
 *
 * Written in scaled integers rather than in floating point, and the
 * difference is not performance.
 *
 * The section 21 formula reduces, for any set of coefficients, to
 *
 *     R = Y + (2 - 2*LumaRed) * Cr
 *     B = Y + (2 - 2*LumaBlue) * Cb
 *     G = Y - (LumaRed/LumaGreen)*(2 - 2*LumaRed)*Cr
 *           - (LumaBlue/LumaGreen)*(2 - 2*LumaBlue)*Cb
 *
 * and every implementation evaluates it with the four multipliers fixed at
 * 16 bits and a half added before the shift. Doing the same in double
 * precision and rounding at the end is *not* the same function: the two
 * disagree by one on the green channel for a handful of samples, because
 * green is the only channel whose multipliers are not exact in five decimal
 * places. Measured before this was written: 2 samples of 1,228,800 on
 * dscf0013.tif and 94 of 325,000 on ycbcr-cat.tif, every one of them green.
 *
 * This library has met that difference before from the other side. Its JPEG
 * decoder uses libjpeg's constants - 0.34414 and 0.71414, rounded to five
 * places for bit-compatibility with every libjpeg since 6b - and the note in
 * tools/oracle/containers/IMAGES records IJG v10 disagreeing with
 * libjpeg-turbo by exactly one, on exactly green, for exactly that reason.
 *
 * TIFF is the case where the rounded constants would be wrong rather than
 * merely different: a TIFF *states* its coefficients in tag 529, so the
 * multipliers are the file's and have to be computed from it. A file that
 * says something other than CCIR 601-1 - and a scanner's file often does -
 * has no libjpeg constant to borrow.
 */
static void tiff_ycbcr_to_rgb(const gimg_tiff_ifd_t * ifd, unsigned y,
    unsigned cb, unsigned cr, unsigned char * out) {
  const double * ref = ifd->reference_black_white;
  const double span_y = (ref[1] - ref[0]) != 0.0 ? (ref[1] - ref[0]) : 255.0;
  const double span_cb = (ref[3] - ref[2]) != 0.0 ? (ref[3] - ref[2]) : 127.0;
  const double span_cr = (ref[5] - ref[4]) != 0.0 ? (ref[5] - ref[4]) : 127.0;
  const double green =
      ifd->luma_green != 0.0 ? ifd->luma_green : 0.587;

  // FIX(x) is x at sixteen fractional bits, rounded, and ONE_HALF is what is
  // added before the arithmetic shift so that the shift rounds rather than
  // floors.
  const double d1 = 2.0 - (2.0 * ifd->luma_red);
  const double d3 = 2.0 - (2.0 * ifd->luma_blue);
  const int32_t fix_r = (int32_t)((d1 * 65536.0) + 0.5);
  const int32_t fix_b = (int32_t)((d3 * 65536.0) + 0.5);
  const int32_t fix_gr =
      -(int32_t)(((ifd->luma_red / green) * d1 * 65536.0) + 0.5);
  const int32_t fix_gb =
      -(int32_t)(((ifd->luma_blue / green) * d3 * 65536.0) + 0.5);
  const int32_t one_half = 1 << 15;

  // The reference levels scale the samples before any of that; with the
  // defaults this is the identity and Cb and Cr simply lose their 128.
  const int32_t yy =
      (int32_t)((((double)y - ref[0]) * 255.0 / span_y) + 0.5);
  const int32_t cbb =
      (int32_t)(((double)cb - ref[2]) * 127.0 / span_cb +
          (((double)cb - ref[2]) < 0.0 ? -0.5 : 0.5));
  const int32_t crr =
      (int32_t)(((double)cr - ref[4]) * 127.0 / span_cr +
          (((double)cr - ref[4]) < 0.0 ? -0.5 : 0.5));

  const int32_t r = yy + (int32_t)(((fix_r * crr) + one_half) >> 16);
  const int32_t b = yy + (int32_t)(((fix_b * cbb) + one_half) >> 16);
  const int32_t g =
      yy + (int32_t)((((fix_gr * crr) + (fix_gb * cbb)) + one_half) >> 16);
  const int32_t v[3] = {r, g, b};
  for (size_t k = 0; k < 3u; k++) {
    // Clamped: the conversion can leave the cube for a chroma pair no
    // encoder would produce from a real colour, and a file may carry one.
    out[k] = v[k] <= 0 ? 0u : (v[k] >= 255 ? 255u : (unsigned char)v[k]);
  }
}

/**
 * Assemble one block of a YCbCr image.
 *
 * YCbCr is the one photometric here whose pixels are not stored a row at a
 * time. The image is divided into *subsampling units* of h by v luma samples
 * (section 21), and a unit is stored as its h*v luma values followed by one
 * Cb and one Cr - so a unit row spans v image rows and the whole block has
 * to be walked as a grid of units rather than as a sequence of rows.
 *
 * The 1x1 case falls out of the same loop: a unit is then one Y, one Cb and
 * one Cr, which is plain interleaved YCbCr.
 */
static void tiff_convert_ycbcr_block(const gimg_tiff_ifd_t * ifd,
    const tiff_output_t * out, const unsigned char * src, size_t have,
    const tiff_block_t * rect, size_t across, size_t down,
    unsigned char * pixels, size_t stride) {
  const size_t h = ifd->ycbcr_h;
  const size_t v = ifd->ycbcr_v;
  const size_t units_across = ((size_t)rect->width + h - 1u) / h;
  const size_t unit_bytes = (h * v) + 2u;
  const size_t unit_row_bytes = units_across * unit_bytes;
  if (unit_row_bytes == 0u) {
    return;
  }
  const size_t unit_rows = (down + v - 1u) / v;

  for (size_t uy = 0; uy < unit_rows; uy++) {
    // A block shorter than its geometry says is truncated data, not a reason
    // to refuse the picture: the unit rows that arrived are kept.
    if ((uy + 1u) * unit_row_bytes > have) {
      return;
    }
    const unsigned char * urow = src + (uy * unit_row_bytes);
    for (size_t ux = 0; ux < units_across; ux++) {
      const unsigned char * unit = urow + (ux * unit_bytes);
      const unsigned cb = unit[h * v];
      const unsigned cr = unit[(h * v) + 1u];
      for (size_t iy = 0; iy < v; iy++) {
        const size_t row = (uy * v) + iy;
        if (row >= down) {
          break;
        }
        unsigned char * dst_row = pixels +
            ((size_t)(rect->y + row) * stride) +
            ((size_t)rect->x * out->bytes);
        for (size_t ix = 0; ix < h; ix++) {
          const size_t col = (ux * h) + ix;
          if (col >= across) {
            break;
          }
          unsigned char rgb[3];
          tiff_ycbcr_to_rgb(ifd, unit[(iy * h) + ix], cb, cr, rgb);
          unsigned char * p = dst_row + (col * out->bytes);
          p[0] = rgb[0];
          p[1] = rgb[1];
          p[2] = rgb[2];
          p[3] = 255u;
        }
      }
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

/**
 * libtiff's CIELab-to-sRGB tables.
 *
 * The range, the gamma and the matrix are the ones in its RGBA reader
 * (`display_sRGB`, `CIELABTORGB_TABLE_RANGE`). A CIE formula that is more
 * precise disagrees with that reader by a level, which is the defect the
 * YCbCr path already refuses to have. The green and blue steps are computed
 * from the red gun's span because that is what `TIFFCIELabToRGBInit` does;
 * with this display every gun has the same span, so the copy is exact.
 */
#define GIMG_TIFF_LAB_TABLE 1500

typedef struct {
  float yr2r[GIMG_TIFF_LAB_TABLE + 1];
  float yg2g[GIMG_TIFF_LAB_TABLE + 1];
  float yb2b[GIMG_TIFF_LAB_TABLE + 1];
  float x0, y0, z0;
  float step;
} tiff_lab_t;

static bool tiff_lab_init(tiff_lab_t * lab, const gimg_tiff_ifd_t * ifd) {
  // Absent tag 318 is CIE D50, as chromaticities, which is what
  // TIFFGetFieldDefaulted returns. Stated, the tag is used the same way:
  // Y is 100 and X and Z are recovered from x and y.
  float wx, wy;
  if (ifd->has_white_point) {
    wx = (float)ifd->white_point.x;
    wy = (float)ifd->white_point.y;
  }
  else {
    const float d50_x = 96.4250F;
    const float d50_y = 100.0F;
    const float d50_z = 82.4680F;
    const float sum = d50_x + d50_y + d50_z;
    wx = d50_x / sum;
    wy = d50_y / sum;
  }
  if (wy == 0.0F) {
    return false;
  }
  lab->y0 = 100.0F;
  lab->x0 = wx / wy * lab->y0;
  lab->z0 = (1.0F - wx - wy) / wy * lab->y0;

  const double inv_gamma = 1.0 / 2.4;
  lab->step = (100.0F - 1.0F) / (float)GIMG_TIFF_LAB_TABLE;
  for (size_t i = 0; i <= (size_t)GIMG_TIFF_LAB_TABLE; i++) {
    const float v = 255.0F *
        (float)pow((double)i / (double)GIMG_TIFF_LAB_TABLE, inv_gamma);
    lab->yr2r[i] = v;
    lab->yg2g[i] = v;
    lab->yb2b[i] = v;
  }
  return true;
}

/** One gun, by libtiff's `RINT` of the gamma table. */
static uint8_t tiff_lab_gun(float y, float black, float white, float step,
    const float * table) {
  if (y < black) {
    y = black;
  }
  if (y > white) {
    y = white;
  }
  size_t i = (size_t)((y - black) / step);
  if (i > (size_t)GIMG_TIFF_LAB_TABLE) {
    i = (size_t)GIMG_TIFF_LAB_TABLE;
  }
  const float r = table[i];
  uint32_t u = (uint32_t)(r > 0.0F ? (r + 0.5) : (r - 0.5));
  if (u > 255u) {
    u = 255u;
  }
  return (uint8_t)u;
}

/**
 * One L\*a\*b\* triple in the sixteen-bit CIE encoding `TIFFCIELab16ToXYZ`
 * takes: L in 0..65535, a\* and b\* already multiplied by 256.
 */
static void tiff_lab16_to_rgb(const tiff_lab_t * lab, uint32_t l, int32_t a,
    int32_t b, uint8_t rgb[3]) {
  const float L = (float)l * 100.0F / 65535.0F;
  float Y, cby, X, Z, tmp;
  if (L < 8.856F) {
    Y = (L * lab->y0) / 903.292F;
    cby = 7.787F * (Y / lab->y0) + 16.0F / 116.0F;
  }
  else {
    cby = (L + 16.0F) / 116.0F;
    Y = lab->y0 * cby * cby * cby;
  }
  tmp = (float)a / 256.0F / 500.0F + cby;
  if (tmp < 0.2069F) {
    X = lab->x0 * (tmp - 0.13793F) / 7.787F;
  }
  else {
    X = lab->x0 * tmp * tmp * tmp;
  }
  tmp = cby - (float)b / 256.0F / 200.0F;
  if (tmp < 0.2069F) {
    Z = lab->z0 * (tmp - 0.13793F) / 7.787F;
  }
  else {
    Z = lab->z0 * tmp * tmp * tmp;
  }

  static const float m[9] = {
      3.2410F, -1.5374F, -0.4986F, -0.9692F, 1.8760F, 0.0416F, 0.0556F,
      -0.2040F, 1.0570F,
  };
  const float Yr = m[0] * X + m[1] * Y + m[2] * Z;
  const float Yg = m[3] * X + m[4] * Y + m[5] * Z;
  const float Yb = m[6] * X + m[7] * Y + m[8] * Z;
  rgb[0] = tiff_lab_gun(Yr, 1.0F, 100.0F, lab->step, lab->yr2r);
  rgb[1] = tiff_lab_gun(Yg, 1.0F, 100.0F, lab->step, lab->yg2g);
  rgb[2] = tiff_lab_gun(Yb, 1.0F, 100.0F, lab->step, lab->yb2b);
}

/**
 * File samples as the arguments of `TIFFCIELab16ToXYZ`.
 *
 * Photometric 8 stores a\* and b\* signed. Photometric 9 stores them with
 * 128 or 32768 added, and its sixteen-bit L\* reaches 100 at 65280 rather
 * than at 65535, so that sample is scaled onto the 65535-based function.
 * An eight-bit pixel and the sixteen-bit pixel `L*257`, `a*256`, `b*256`
 * are the same arguments, which is what makes the two depths agree.
 */
static void tiff_lab_arguments(const gimg_tiff_ifd_t * ifd, uint32_t s0,
    uint32_t s1, uint32_t s2, uint32_t * l, int32_t * a, int32_t * b) {
  const bool icc = ifd->photometric == GIMG_TIFF_PHOTOMETRIC_ICCLAB;
  if (ifd->bits_per_sample == 8u) {
    const int32_t aa =
        icc ? (int32_t)s1 - 128 : (int32_t)(int8_t)(uint8_t)s1;
    const int32_t bb =
        icc ? (int32_t)s2 - 128 : (int32_t)(int8_t)(uint8_t)s2;
    *l = s0 * 257u;
    *a = aa * 256;
    *b = bb * 256;
    return;
  }
  if (icc) {
    *l = (uint32_t)(((uint64_t)s0 * 65535u) / 65280u);
    *a = (int32_t)s1 - 32768;
    *b = (int32_t)s2 - 32768;
    return;
  }
  *l = s0;
  *a = (int32_t)(int16_t)s1;
  *b = (int32_t)(int16_t)s2;
}

/** One interleaved Lab row, written as RGBA8. Alpha is filled later. */
static void tiff_convert_lab_row(const gimg_tiff_ifd_t * ifd,
    const tiff_lab_t * lab, const unsigned char * src, unsigned char * dst,
    size_t pixels) {
  const unsigned bits = ifd->bits_per_sample;
  const bool be = ifd->file_big_endian;
  const size_t spp = ifd->samples_per_pixel;
  for (size_t i = 0; i < pixels; i++) {
    const uint32_t s0 = tiff_sample(src, i * spp, bits, be);
    const uint32_t s1 = tiff_sample(src, i * spp + 1u, bits, be);
    const uint32_t s2 = tiff_sample(src, i * spp + 2u, bits, be);
    uint32_t l = 0;
    int32_t a = 0, b = 0;
    tiff_lab_arguments(ifd, s0, s1, s2, &l, &a, &b);
    unsigned char * p = dst + (i * 4u);
    tiff_lab16_to_rgb(lab, l, a, b, p);
    if (spp >= 4u) {
      p[3] = tiff_to_8(tiff_sample(src, i * spp + 3u, bits, be), bits);
    }
  }
}

/**
 * One plane of a Lab row, stored raw.
 *
 * The conversion needs L\*, a\* and b\* together, and with
 * PlanarConfiguration 2 they arrive in different blocks, so the samples wait
 * here until every plane has been seen. Converting from this buffer with the
 * same function the interleaved path uses is what keeps the two layouts from
 * drifting.
 */
static void tiff_stash_lab_row(const gimg_tiff_ifd_t * ifd, uint16_t * raw,
    const unsigned char * src, uint32_t x, uint32_t y, size_t pixels,
    int plane) {
  const unsigned bits = ifd->bits_per_sample;
  const bool be = ifd->file_big_endian;
  const size_t spp = ifd->samples_per_pixel;
  if (plane < 0 || (size_t)plane >= spp) {
    return;
  }
  for (size_t i = 0; i < pixels; i++) {
    const size_t at =
        (((size_t)y * ifd->width) + (size_t)x + i) * spp + (size_t)plane;
    raw[at] = (uint16_t)tiff_sample(src, i, bits, be);
  }
}

/** The stashed planes, through the same conversion as an interleaved row. */
static void tiff_lab_from_planes(const gimg_tiff_ifd_t * ifd,
    const tiff_lab_t * lab, const uint16_t * raw, unsigned char * dst,
    size_t stride) {
  const size_t spp = ifd->samples_per_pixel;
  const unsigned bits = ifd->bits_per_sample;
  for (uint32_t y = 0; y < ifd->height; y++) {
    unsigned char * row = dst + ((size_t)y * stride);
    for (uint32_t x = 0; x < ifd->width; x++) {
      const uint16_t * s =
          raw + ((((size_t)y * ifd->width) + x) * spp);
      uint32_t l = 0;
      int32_t a = 0, b = 0;
      tiff_lab_arguments(ifd, s[0], s[1], s[2], &l, &a, &b);
      unsigned char * p = row + ((size_t)x * 4u);
      tiff_lab16_to_rgb(lab, l, a, b, p);
      if (spp >= 4u) {
        p[3] = tiff_to_8(s[3], bits);
      }
    }
  }
}

static GIMG_Result tiff_decode_fail(const GIMG_Allocator * alloc,
    GIMG_Raster * raster, void * lab, void * raw, GIMG_Result r) {
  gimg_free(alloc, lab);
  gimg_free(alloc, raw);
  gimg_raster_destroy(raster);
  return r;
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
  const bool lab = ifd->photometric == GIMG_TIFF_PHOTOMETRIC_CIELAB ||
      ifd->photometric == GIMG_TIFF_PHOTOMETRIC_ICCLAB;
  tiff_lab_t * lab_state = NULL;
  uint16_t * lab_raw = NULL;
  if (lab) {
    lab_state = (tiff_lab_t *)gimg_malloc(alloc, sizeof(*lab_state));
    if (!lab_state) {
      gimg_raster_destroy(raster);
      return GIMG_ERR_OOM;
    }
    if (!tiff_lab_init(lab_state, ifd)) {
      return tiff_decode_fail(
          alloc, raster, lab_state, NULL, GIMG_ERR_CORRUPT);
    }
    if (ifd->planar_config == 2u) {
      size_t count = 0;
      size_t bytes = 0;
      if (!gcu_safe_mul_size(pixels, ifd->samples_per_pixel, &count) ||
          !gcu_safe_mul_size(count, sizeof(uint16_t), &bytes)) {
        return tiff_decode_fail(
            alloc, raster, lab_state, NULL, GIMG_ERR_LIMIT);
      }
      lab_raw = (uint16_t *)gimg_malloc(alloc, bytes);
      if (!lab_raw) {
        return tiff_decode_fail(alloc, raster, lab_state, NULL, GIMG_ERR_OOM);
      }
      memset(lab_raw, 0, bytes);
    }
  }

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
    if (ifd->photometric == GIMG_TIFF_PHOTOMETRIC_YCBCR) {
      // A unit row spans YCbCrSubSampling[1] image rows and holds one unit
      // per YCbCrSubSampling[0] columns, each unit being its luma samples
      // plus one Cb and one Cr.
      const size_t h = ifd->ycbcr_h, v = ifd->ycbcr_v;
      const size_t units = ((size_t)rect.width + h - 1u) / h;
      const size_t unit_rows = (down + v - 1u) / v;
      if (!gcu_safe_mul_size(units, (h * v) + 2u, &want) ||
          !gcu_safe_mul_size(want, unit_rows, &want)) {
        return tiff_decode_fail(
            alloc, raster, lab_state, lab_raw, GIMG_ERR_LIMIT);
      }
    }
    else if (!gcu_safe_mul_size(down, rect.row_bytes, &want)) {
      return tiff_decode_fail(
          alloc, raster, lab_state, lab_raw, GIMG_ERR_LIMIT);
    }
    if (ifd->compression == GIMG_TIFF_COMPRESSION_JPEG ||
        ifd->compression == GIMG_TIFF_COMPRESSION_JPEG_OLD) {
      // A JPEG strip decodes to a picture rather than to rows of samples, so
      // it is copied in whole here instead of going through the row
      // converter. The formats line up by construction: this codec's JPEG
      // decoder produces GRAY8, RGBA8 or CMYK8, and tiff_plan_output chose
      // from the same three.
      GIMG_Raster * part = NULL;
      r = gimg_tiff_jpeg_block(st, ifd, b, limits, &part);
      if (r != GIMG_OK) {
        return tiff_decode_fail(alloc, raster, lab_state, lab_raw, r);
      }
      const GIMG_Pixel_Format * pf = gimg_raster_format(part);
      if (!pf || pf->channel_count != out.channels ||
          gimg_pixel_format_channel_bits(pf, 0) != 8u) {
        // The strip's frame header disagrees with the directory about how
        // many components the picture has. Refused rather than copied
        // channel by channel: the two halves of the file describe different
        // images and there is no saying which is the picture.
        gimg_raster_destroy(part);
        return tiff_decode_fail(
            alloc, raster, lab_state, lab_raw, GIMG_ERR_CORRUPT);
      }
      const size_t part_stride = gimg_raster_stride_bytes(part);
      const unsigned char * part_px =
          (const unsigned char *)gimg_raster_pixels(part);
      const size_t part_w = gimg_raster_width(part);
      const size_t part_h = gimg_raster_height(part);
      const size_t copy_w = across < part_w ? across : part_w;
      const size_t copy_h = down < part_h ? down : part_h;
      for (size_t row = 0; row < copy_h; row++) {
        memcpy(dst_pixels + ((rect.y + row) * stride) + (rect.x * out.bytes),
            part_px + (row * part_stride), copy_w * out.bytes);
      }
      gimg_raster_destroy(part);
      continue;
    }
    const unsigned char * src = NULL;
    size_t have = 0;
    bool owned = false;
    r = gimg_tiff_block_bytes(st, ifd, b, want, &src, &have, &owned);
    if (r != GIMG_OK) {
      return tiff_decode_fail(alloc, raster, lab_state, lab_raw, r);
    }
    if (owned) {
      // The predictor is undone over the expanded block, before any row of
      // it is read, because a difference is relative to the sample before it
      // and the row converter works one row at a time.
      gimg_tiff_undo_block_predictor(ifd, (unsigned char *)(uintptr_t)src,
          have, rect.row_bytes,
          rect.plane >= 0 ? 1u : ifd->samples_per_pixel);
    }

    if (ifd->photometric == GIMG_TIFF_PHOTOMETRIC_YCBCR) {
      tiff_convert_ycbcr_block(
          ifd, &out, src, have, &rect, across, down, dst_pixels, stride);
      if (owned) {
        gimg_free(alloc, (void *)(uintptr_t)src);
      }
      continue;
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
      if (lab_raw) {
        tiff_stash_lab_row(ifd, lab_raw, src + at, rect.x,
            (uint32_t)(rect.y + row), across, rect.plane);
        continue;
      }
      unsigned char * dst = dst_pixels +
          ((size_t)(rect.y + row) * stride) + ((size_t)rect.x * out.bytes);
      if (lab_state) {
        tiff_convert_lab_row(ifd, lab_state, src + at, dst, across);
      }
      else {
        tiff_convert_row(ifd, &out, src + at, dst, across, rect.plane);
      }
    }
    if (owned) {
      gimg_free(alloc, (void *)(uintptr_t)src);
    }
  }

  if (lab_raw) {
    tiff_lab_from_planes(ifd, lab_state, lab_raw, dst_pixels, stride);
  }
  gimg_free(alloc, lab_raw);
  gimg_free(alloc, lab_state);

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
  const bool has_icc = ifd->icc && ifd->icc_size > 0u;
  // TIFF states the white point in its own tag, so a file may carry one
  // without the other.  The two flags are set independently: the primaries
  // are what make a gamut, and a lone white point is carried without
  // claiming to be one - which is what stops the PNG writer emitting a cHRM
  // whose three primaries are (0, 0).
  //
  // A Lab page is the exception. Tag 318 there is the reference white of the
  // conversion already performed, and the file's primaries are not the sRGB
  // primaries that conversion used. Attaching either would describe a
  // different colour space from the bytes. An ICC profile still applies.
  if (has_icc || (!lab && (ifd->has_primaries || ifd->has_white_point))) {
    GCOL_Color_Info info;
    gcol_color_info_default(&info);
    const GCOL_Color_Info * existing = gimg_raster_color_info_const(raster);
    if (existing) {
      info = *existing;
    }
    if (!lab && ifd->has_primaries) {
      info.gamut.red = ifd->primaries[0];
      info.gamut.green = ifd->primaries[1];
      info.gamut.blue = ifd->primaries[2];
      info.primaries_stated = true;
    }
    if (!lab && ifd->has_white_point) {
      info.gamut.white = ifd->white_point;
      info.white_stated = true;
    }
    if (has_icc) {
      info.icc_bytes = ifd->icc;
      info.icc_size = ifd->icc_size;
    }
    // set_color_info copies the profile, which is why the IFD may keep
    // owning these bytes and the raster may outlive nothing.
    (void)gimg_raster_set_color_info(raster, &info);
  }

  *out_raster = raster;
  return GIMG_OK;
}

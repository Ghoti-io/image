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
 * Internal types for the TIFF codec (TIFF 6.0).
 *
 * The whole file is held in memory while a document is open.  TIFF addresses
 * everything - every tag value too large for four bytes, every strip, every
 * tile, every further IFD - by absolute file offset, and those offsets may
 * point backwards.  A reader that seeks per value would seek thousands of
 * times for a file it could have read once, and could not work at all on a
 * source that cannot seek, which is why the loader refuses one.
 */

#ifndef GHOTI_IO_GIMG_SRC_CODEC_TIFF_TIFF_INTERNAL_H
#define GHOTI_IO_GIMG_SRC_CODEC_TIFF_TIFF_INTERNAL_H

#include <ghoti.io/image/macros.h>

#include <ghoti.io/image/allocator.h>
#include <ghoti.io/image/codec.h>
#include <ghoti.io/image/core.h>
#include <ghoti.io/image/doc.h>
#include <ghoti.io/image/stream.h>
#include <stdbool.h>
#include <stdint.h>

/** Length of every TIFF header magic: order, then version. */
#define GIMG_TIFF_SIGNATURE_LEN 4

/** "II" then 42: little-endian classic TIFF (6.0 section 2). */
extern const unsigned char gimg_tiff_magic_le[GIMG_TIFF_SIGNATURE_LEN];
/** "MM" then 42: big-endian classic TIFF. */
extern const unsigned char gimg_tiff_magic_be[GIMG_TIFF_SIGNATURE_LEN];
/** "II" then 43: little-endian BigTIFF, recognised only to be refused. */
extern const unsigned char gimg_tiff_magic_le_big[GIMG_TIFF_SIGNATURE_LEN];
/** "MM" then 43: big-endian BigTIFF, likewise. */
extern const unsigned char gimg_tiff_magic_be_big[GIMG_TIFF_SIGNATURE_LEN];

/** @name TIFF 6.0 field types (section 2, "Types") @{ */
#define GIMG_TIFF_TYPE_BYTE 1
#define GIMG_TIFF_TYPE_ASCII 2
#define GIMG_TIFF_TYPE_SHORT 3
#define GIMG_TIFF_TYPE_LONG 4
#define GIMG_TIFF_TYPE_RATIONAL 5
#define GIMG_TIFF_TYPE_SBYTE 6
#define GIMG_TIFF_TYPE_UNDEFINED 7
#define GIMG_TIFF_TYPE_SSHORT 8
#define GIMG_TIFF_TYPE_SLONG 9
#define GIMG_TIFF_TYPE_SRATIONAL 10
#define GIMG_TIFF_TYPE_FLOAT 11
#define GIMG_TIFF_TYPE_DOUBLE 12
#define GIMG_TIFF_TYPE_MAX 12
/** @} */

/** @name The baseline tags this codec reads (TIFF 6.0 sections 8 and 15) @{ */
#define GIMG_TIFF_TAG_NEW_SUBFILE_TYPE 254
#define GIMG_TIFF_TAG_IMAGE_WIDTH 256
#define GIMG_TIFF_TAG_IMAGE_LENGTH 257
#define GIMG_TIFF_TAG_BITS_PER_SAMPLE 258
#define GIMG_TIFF_TAG_COMPRESSION 259
#define GIMG_TIFF_TAG_PHOTOMETRIC 262
#define GIMG_TIFF_TAG_STRIP_OFFSETS 273
#define GIMG_TIFF_TAG_SAMPLES_PER_PIXEL 277
#define GIMG_TIFF_TAG_ROWS_PER_STRIP 278
#define GIMG_TIFF_TAG_STRIP_BYTE_COUNTS 279
#define GIMG_TIFF_TAG_X_RESOLUTION 282
#define GIMG_TIFF_TAG_Y_RESOLUTION 283
#define GIMG_TIFF_TAG_PLANAR_CONFIG 284
#define GIMG_TIFF_TAG_RESOLUTION_UNIT 296
#define GIMG_TIFF_TAG_COLOR_MAP 320
#define GIMG_TIFF_TAG_TILE_WIDTH 322
#define GIMG_TIFF_TAG_TILE_LENGTH 323
#define GIMG_TIFF_TAG_TILE_OFFSETS 324
#define GIMG_TIFF_TAG_TILE_BYTE_COUNTS 325
#define GIMG_TIFF_TAG_EXTRA_SAMPLES 338
#define GIMG_TIFF_TAG_SAMPLE_FORMAT 339
/** @} */

/** @name Compression values (TIFF 6.0 section 8) @{ */
#define GIMG_TIFF_COMPRESSION_NONE 1
#define GIMG_TIFF_COMPRESSION_CCITT_RLE 2
#define GIMG_TIFF_COMPRESSION_CCITT_T4 3
#define GIMG_TIFF_COMPRESSION_CCITT_T6 4
#define GIMG_TIFF_COMPRESSION_LZW 5
#define GIMG_TIFF_COMPRESSION_JPEG_OLD 6
#define GIMG_TIFF_COMPRESSION_JPEG 7
#define GIMG_TIFF_COMPRESSION_DEFLATE 8
#define GIMG_TIFF_COMPRESSION_PACKBITS 32773
/** @} */

/** @name PhotometricInterpretation (TIFF 6.0 section 8) @{ */
#define GIMG_TIFF_PHOTOMETRIC_WHITE_IS_ZERO 0
#define GIMG_TIFF_PHOTOMETRIC_BLACK_IS_ZERO 1
#define GIMG_TIFF_PHOTOMETRIC_RGB 2
#define GIMG_TIFF_PHOTOMETRIC_PALETTE 3
#define GIMG_TIFF_PHOTOMETRIC_TRANSPARENCY_MASK 4
#define GIMG_TIFF_PHOTOMETRIC_CMYK 5
#define GIMG_TIFF_PHOTOMETRIC_YCBCR 6
/** @} */

/** ExtraSamples (TIFF 6.0 section 18): what a fourth or later sample means. */
#define GIMG_TIFF_EXTRA_UNSPECIFIED 0
#define GIMG_TIFF_EXTRA_ASSOCIATED_ALPHA 1
#define GIMG_TIFF_EXTRA_UNASSOCIATED_ALPHA 2

/**
 * How many IFDs one file may chain.
 *
 * The chain is refused if it does not move forward, which already makes a
 * cycle impossible, but a file can still name a great many tiny IFDs; this
 * bounds the work and the allocation that follows from one header.  Real
 * multi-page documents are far under it.
 */
#define GIMG_TIFF_MAX_IFDS 4096u

/** How many entries one IFD may hold.  The count is a 16-bit field, so this
 * is its own ceiling rather than a policy. */
#define GIMG_TIFF_MAX_ENTRIES 65535u

/** One image file directory, normalized: every default resolved, every array
 * copied out of the file. */
typedef struct {
  uint32_t width;
  uint32_t height;
  uint16_t bits_per_sample; ///< Uniform across samples; a file that varies
                            ///< them is refused.
  uint16_t samples_per_pixel;
  uint16_t photometric;
  uint16_t compression;
  uint16_t planar_config;
  uint16_t sample_format;
  uint16_t resolution_unit;
  uint16_t extra_samples; ///< First ExtraSamples value; meaningful only when
                          ///< has_extra_samples.
  bool has_extra_samples;
  bool tiled; ///< True when TileWidth and TileLength are present.
  uint32_t rows_per_strip;
  uint32_t tile_width;
  uint32_t tile_height;
  /** Where each strip or tile begins, and how long it is.  One array pair for
   * both cases: a strip is a tile as wide as the image. */
  uint64_t * block_offsets;
  uint64_t * block_byte_counts;
  size_t block_count;
  /** ColorMap, as the file stores it: all reds, then all greens, then all
   * blues, each 16-bit (TIFF 6.0 section 8). */
  uint16_t * color_map;
  size_t color_map_count;
  uint32_t x_res_num, x_res_den;
  uint32_t y_res_num, y_res_den;
  bool has_x_res, has_y_res;
} gimg_tiff_ifd_t;

/** Document-private state: the file, and one entry per IFD. */
typedef struct {
  const GIMG_Allocator * allocator;
  bool big_endian;
  unsigned char * file; ///< The whole file, owned.
  size_t file_size;
  gimg_tiff_ifd_t * ifds;
  size_t ifd_count;
} gimg_tiff_doc_state_t;

/** Read the header and say which byte order it declares.
 * @return GIMG_OK with @p out_big_endian set, GIMG_ERR_FORMAT if the bytes
 *   are not a TIFF header, GIMG_ERR_UNSUPPORTED for BigTIFF. */
GIMG_Result gimg_tiff_read_header(
    const unsigned char * bytes, size_t size, bool * out_big_endian);

GIMG_Result gimg_tiff_load(GIMG_Codec * codec, GIMG_Stream * stream,
    const GIMG_Load_Options * options, GIMG_Diagnostics * diagnostics,
    GIMG_Doc ** out_doc);

GIMG_Result gimg_tiff_decode(GIMG_Codec * codec, const GIMG_Item * item,
    const GIMG_Decode_Options * options, GIMG_Raster ** out_raster);

void gimg_tiff_free_doc_state(GIMG_Codec * codec, void * codec_private);

#endif // GHOTI_IO_GIMG_SRC_CODEC_TIFF_TIFF_INTERNAL_H

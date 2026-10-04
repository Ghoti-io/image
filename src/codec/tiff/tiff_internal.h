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
#define GIMG_TIFF_TAG_FILL_ORDER 266
#define GIMG_TIFF_TAG_T4_OPTIONS 292
#define GIMG_TIFF_TAG_T6_OPTIONS 293
#define GIMG_TIFF_TAG_PREDICTOR 317
#define GIMG_TIFF_TAG_IMAGE_DESCRIPTION 270
#define GIMG_TIFF_TAG_ORIENTATION 274
#define GIMG_TIFF_TAG_XMP 700
#define GIMG_TIFF_TAG_WHITE_POINT 318
#define GIMG_TIFF_TAG_PRIMARY_CHROMATICITIES 319
#define GIMG_TIFF_TAG_ICC_PROFILE 34675
#define GIMG_TIFF_TAG_SUB_IFDS 330
#define GIMG_TIFF_TAG_JPEG_TABLES 347
/** @name The 1992 JPEG tags, which compression 6 uses and 7 does not @{ */
#define GIMG_TIFF_TAG_JPEG_PROC 512
#define GIMG_TIFF_TAG_JPEG_INTERCHANGE_FORMAT 513
#define GIMG_TIFF_TAG_JPEG_INTERCHANGE_LENGTH 514
#define GIMG_TIFF_TAG_JPEG_RESTART_INTERVAL 515
#define GIMG_TIFF_TAG_JPEG_Q_TABLES 519
#define GIMG_TIFF_TAG_JPEG_DC_TABLES 520
#define GIMG_TIFF_TAG_JPEG_AC_TABLES 521
/** @} */

/** How many per-component table pointers the 1992 tags may name. A frame
 * this codec reads has at most four components, and T.81 B.2.4.1 allows four
 * tables of each class in any case. */
#define GIMG_TIFF_JPEG_MAX_TABLES 4u
#define GIMG_TIFF_TAG_YCBCR_COEFFICIENTS 529
#define GIMG_TIFF_TAG_YCBCR_SUBSAMPLING 530
#define GIMG_TIFF_TAG_REFERENCE_BLACK_WHITE 532
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
#define GIMG_TIFF_COMPRESSION_THUNDERSCAN 32809
/** Deflate again, under the number that predates Adobe's registration. */
#define GIMG_TIFF_COMPRESSION_DEFLATE_OLD 32946
/** @} */

/** @name PhotometricInterpretation (TIFF 6.0 section 8) @{ */
#define GIMG_TIFF_PHOTOMETRIC_WHITE_IS_ZERO 0
#define GIMG_TIFF_PHOTOMETRIC_BLACK_IS_ZERO 1
#define GIMG_TIFF_PHOTOMETRIC_RGB 2
#define GIMG_TIFF_PHOTOMETRIC_PALETTE 3
#define GIMG_TIFF_PHOTOMETRIC_TRANSPARENCY_MASK 4
#define GIMG_TIFF_PHOTOMETRIC_CMYK 5
#define GIMG_TIFF_PHOTOMETRIC_YCBCR 6
/**
 * CIE L\*a\*b\*, TIFF 6.0 section 23. Eight-bit a\* and b\* are signed;
 * sixteen-bit ones are 256 times the 1976 values.
 */
#define GIMG_TIFF_PHOTOMETRIC_CIELAB 8
/**
 * The same L\*a\*b\* with the ICC bias: 128 at eight bits, 32768 at sixteen,
 * and sixteen-bit L\* reaches 100 at 65280. Adobe's TIFF technical note.
 */
#define GIMG_TIFF_PHOTOMETRIC_ICCLAB 9
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
  uint16_t predictor;     ///< 1 = none, 2 = horizontal differencing (TIFF
                          ///< Technical Note 2 / section 14).
  /** FillOrder (266): 1 = most significant bit of a byte first, 2 = least.
   * Only the CCITT decoder reads it; every other compression here is defined
   * on whole bytes, where the question does not arise. */
  uint16_t fill_order;
  uint32_t t4_options; ///< Tag 292. Bit 0 selects two-dimensional coding.
  uint32_t t6_options; ///< Tag 293. Bit 1 allows uncompressed mode.
  bool has_extra_samples;
  bool tiled; ///< True when TileWidth and TileLength are present.
  /** The file's byte order, copied here so a row converter that reads 16-bit
   * samples does not need the document state to find it. */
  bool file_big_endian;
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
  /** True when every entry is below 256 and the map is therefore read as
   * eight-bit values stored in a sixteen-bit field. See tiff_load.c. */
  bool color_map_is_8bit;
  /** Blocks per plane. Equal to block_count unless PlanarConfiguration is 2,
   * where the file holds one set of strips or tiles per sample. */
  size_t blocks_per_plane;
  /** The ICC profile the file carried (tag 34675), owned here. A TIFF is the
   * format professional colour work is stored in, so this is not optional
   * furniture: a file whose profile was dropped is a file whose colours mean
   * something else. */
  unsigned char * icc;
  size_t icc_size;
  /** WhitePoint (318) and PrimaryChromaticities (319), as CIE 1931 x,y.
   *
   * TIFF 6.0 states both as RATIONALs, so they are exact and need no table;
   * gcol_gamut_identify() puts a name to the result if it has one.  A file
   * may carry either alone, so the two flags are separate: a white point
   * with no primaries is not a gamut and is kept only so that a reader is
   * told what the file said.
   *
   * TransferFunction (301) is deliberately not read.  It is a sampled lookup
   * table of 2^BitsPerSample entries, and GCOL_Transfer holds named and
   * parametric curves; fitting a curve to those samples would be inventing a
   * function the file did not state. */
  GCOL_Chromaticity white_point;
  GCOL_Chromaticity primaries[3];
  bool has_white_point;
  bool has_primaries;
  /** JPEGTables (347), owned: the table-specification stream every strip of
   * a compression-7 file is read with. See tiff_jpeg.c. */
  unsigned char * jpeg_tables;
  size_t jpeg_tables_size;
  /** The 1992 JPEG tags. Only compression 6 uses them, and it is the whole
   * reason tiff_ojpeg.c exists: where compression 7 stores a table stream a
   * JPEG decoder can read as it stands, this stores the tables as bare
   * arrays at file offsets, with no frame header anywhere. */
  uint32_t jpeg_proc;
  uint64_t jpeg_interchange_offset;
  uint64_t jpeg_interchange_size;
  uint32_t jpeg_restart_interval;
  uint64_t jpeg_q_tables[GIMG_TIFF_JPEG_MAX_TABLES];
  uint64_t jpeg_dc_tables[GIMG_TIFF_JPEG_MAX_TABLES];
  uint64_t jpeg_ac_tables[GIMG_TIFF_JPEG_MAX_TABLES];
  uint8_t jpeg_q_count;
  uint8_t jpeg_dc_count;
  uint8_t jpeg_ac_count;
  /** ImageDescription (270), NUL-terminated and owned, or NULL. */
  char * description;
  uint16_t orientation;   ///< Tag 274; 0 when the file did not say.
  /** NewSubfileType (254). Bit 0 set means this directory is a
   * reduced-resolution version of another image in the file. */
  uint32_t subfile_type;
  /** What this directory is, as the document model spells it, and which
   * item it is a version of. A page is GIMG_ITEM_IMAGE of itself; a pyramid
   * level is GIMG_ITEM_LEVEL of the full-size picture it belongs to. */
  int role;
  size_t role_subject;
  uint64_t * sub_ifds;    ///< Tag 330: offsets of this page's sub-directories.
  size_t sub_ifd_count;
  /** YCbCr (section 21). The coefficients default to CCIR 601-1's
   * 0.299/0.587/0.114, the subsampling to 2x2, and ReferenceBlackWhite to
   * the range that makes the conversion the ordinary JPEG one. */
  double luma_red, luma_green, luma_blue;
  uint16_t ycbcr_h, ycbcr_v;
  double reference_black_white[6];
  /** XMP (700), owned, kept only so a round trip does not lose it. */
  unsigned char * xmp;
  size_t xmp_size;
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

/**
 * Hand back one block's bytes, decompressed if they were compressed.
 *
 * @param want How many bytes the block's geometry says it holds.
 * @param out_owned Set when the caller must free what came back; a stored
 *   block points straight into the file and owns nothing.
 */
GIMG_Result gimg_tiff_block_bytes(const gimg_tiff_doc_state_t * st,
    const gimg_tiff_ifd_t * ifd, size_t block, size_t want,
    const unsigned char ** out_bytes, size_t * out_size, bool * out_owned);

/** Undo horizontal differencing over one decompressed block, in place. */
void gimg_tiff_undo_block_predictor(const gimg_tiff_ifd_t * ifd,
    unsigned char * data, size_t size, size_t row_bytes, size_t channels);

/**
 * Expand one CCITT Group 3 or Group 4 block into packed one-bit rows.
 *
 * @param out_size The block's uncompressed size, which also says how many
 *   rows it holds. A short or damaged block fills what it can.
 * @return GIMG_OK when at least one row decoded, GIMG_ERR_CORRUPT when none
 *   did.
 */
GIMG_Result gimg_tiff_fax_decode(const GIMG_Allocator * allocator,
    const gimg_tiff_ifd_t * ifd, const unsigned char * src, size_t src_size,
    unsigned char * out, size_t out_size);

/**
 * Decode one strip or tile of a compression-7 file, through this library's
 * own JPEG codec. In tiff_jpeg.c.
 */
GIMG_Result gimg_tiff_jpeg_block(const gimg_tiff_doc_state_t * st,
    const gimg_tiff_ifd_t * ifd, size_t block, const GIMG_Limits * limits,
    GIMG_Raster ** out_raster);

/**
 * Build a complete JPEG datastream for one block of a compression-6 file.
 *
 * @param rows How many image rows the block covers, which is what its frame
 *   header must declare.
 * @param out_stream Owned by the caller, freed with the allocator.
 * @return GIMG_OK, or GIMG_ERR_UNSUPPORTED for a file whose 1992 tags do not
 *   describe a frame this can assemble.
 */
/**
 * Copy a compression-6 interchange stream, correcting the scan header the
 * 1992 spelling left meaningless. In tiff_ojpeg.c.
 */
GIMG_Result gimg_tiff_ojpeg_fix_interchange(const gimg_tiff_doc_state_t * st,
    const unsigned char * bytes, size_t size, unsigned char ** out_stream,
    size_t * out_size);

GIMG_Result gimg_tiff_ojpeg_assemble(const gimg_tiff_doc_state_t * st,
    const gimg_tiff_ifd_t * ifd, size_t block, uint32_t rows,
    unsigned char ** out_stream, size_t * out_size);

/**
 * Expand one ThunderScan block into packed four-bit rows. In tiff_thunder.c.
 */
GIMG_Result gimg_tiff_thunder_decode(const gimg_tiff_ifd_t * ifd,
    const unsigned char * src, size_t src_size, unsigned char * out,
    size_t out_size);

/** Whether this codec can undo @p compression. */
bool gimg_tiff_compression_known(uint16_t compression);

GIMG_Result gimg_tiff_save(GIMG_Codec * codec, const GIMG_Doc * doc,
    GIMG_Stream * stream, const char * format_name,
    const GIMG_Save_Options * options, GIMG_Save_Report * report);

GIMG_Result gimg_tiff_decode(GIMG_Codec * codec, const GIMG_Item * item,
    const GIMG_Decode_Options * options, GIMG_Raster ** out_raster);

void gimg_tiff_free_doc_state(GIMG_Codec * codec, void * codec_private);

#endif // GHOTI_IO_GIMG_SRC_CODEC_TIFF_TIFF_INTERNAL_H

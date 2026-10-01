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
 * Codec registry, probing, load/save API (spec §7).
 */

#ifndef GHOTI_IO_GIMG_CODEC_H
#define GHOTI_IO_GIMG_CODEC_H

#include <ghoti.io/image/allocator.h>
#include <ghoti.io/image/core.h>
#include <ghoti.io/image/doc.h>
#include <ghoti.io/image/macros.h>
#include <ghoti.io/image/meta.h>
#include <ghoti.io/image/raster.h>
#include <ghoti.io/image/stream.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Opaque codec descriptor (name, probe, capabilities). */
typedef struct GIMG_Codec GIMG_Codec;

/**
 * @brief Codec capability bits (bitmask for codec->capabilities).
 */
#define GIMG_CAP_READ (1u << 0)
#define GIMG_CAP_WRITE (1u << 1)
#define GIMG_CAP_ANIMATION (1u << 2)
#define GIMG_CAP_PALETTE (1u << 3)
#define GIMG_CAP_ICC (1u << 4)
#define GIMG_CAP_16BPC (1u << 5)
#define GIMG_CAP_CMYK (1u << 6)

/**
 * @brief Probe result: likely format name and confidence.
 *
 * Lifetime: format_name is valid only until the next call that mutates the
 * codec registry (e.g. gimg_codec_register()). Do not store the pointer
 * long-term; copy the string if you need to keep it.
 */
typedef struct {
  const char * format_name; ///< The name of the codec that claimed the bytes,
                            ///< as gimg_codec_name() reports it; NULL if none
                            ///< did.
  unsigned int confidence;  ///< 0–100; 0 = no match.
  uint8_t _reserved[4];
} GIMG_Probe_Result;

/**
 * @brief Create a stub codec for registration (uses default allocator).
 */
GIMG_API GIMG_Result gimg_codec_create_stub(const char * name,
    const void * magic_bytes, size_t magic_len, GIMG_Codec ** out_codec);

/**
 * @brief Create a stub codec with a specific allocator.
 * @param allocator Allocator for codec and its name/magic copies (NULL =
 * default).
 */
GIMG_API GIMG_Result gimg_codec_create_stub_with_allocator(
    const GIMG_Allocator * allocator, const char * name,
    const void * magic_bytes, size_t magic_len, GIMG_Codec ** out_codec);

/**
 * @brief Register a codec (by name, probe, capabilities).
 * @param codec Codec to register (library takes ownership of pointer).
 * @return GIMG_OK or GIMG_ERR_OOM / duplicate name.
 */
GIMG_API GIMG_Result gimg_codec_register(GIMG_Codec * codec);

/**
 * @brief Get number of registered codecs.
 */
GIMG_API size_t gimg_codec_count(void);

/**
 * @brief Get codec by index (0 .. count-1).
 */
GIMG_API GIMG_Codec * gimg_codec_by_index(size_t index);

/**
 * @brief Get codec by name (NULL if not found).
 */
GIMG_API GIMG_Codec * gimg_codec_by_name(const char * name);

/**
 * @brief Get codec display name.
 */
GIMG_API const char * gimg_codec_name(const GIMG_Codec * codec);

/**
 * @brief Get codec capability bitmask (GIMG_CAP_*).
 */
GIMG_API unsigned int gimg_codec_capabilities(const GIMG_Codec * codec);

/**
 * @brief Probe stream to identify format (peek where possible).
 * @param stream Stream to probe.
 * @param result Filled with format name and confidence.
 * @return GIMG_OK; result->format_name NULL if no codec matched.
 */
GIMG_API GIMG_Result gimg_probe(
    GIMG_Stream * stream, GIMG_Probe_Result * result);

/**
 * @brief A set of JPEG tables read from a table-specification stream.
 *
 * ISO/IEC 10918-1 (T.81) B.4 describes two abbreviated formats.  One is a
 * stream of table-specification data with no frame in it - DQT, DHT, DAC and
 * DRI between SOI and EOI - which installs tables for later use; the other is
 * a stream carrying a frame whose tables are missing, to be read with the
 * tables that stream installed.  The pair exists so that a set of images can
 * share one copy of its tables, and neither half is a complete JPEG on its
 * own.
 *
 * Load one with gimg_jpeg_tables_load(), pass it in
 * GIMG_Load_Options.jpeg_tables to read an abbreviated image, and free it with
 * gimg_jpeg_tables_destroy().  One tables object may be used for any number of
 * loads; it is not modified by them.
 */
typedef struct GIMG_JPEG_Tables GIMG_JPEG_Tables;

/**
 * @brief Read a JPEG table-specification stream (T.81 B.4).
 *
 * The stream must be SOI, table-specification and miscellaneous segments, then
 * EOI, with no frame header.  A stream that carries a frame is not a
 * table-specification stream and returns GIMG_ERR_FORMAT.
 */
GIMG_API GIMG_Result gimg_jpeg_tables_load(
    GIMG_Stream * stream, GIMG_JPEG_Tables ** out_tables);

/** @brief Free a table set from gimg_jpeg_tables_load(). */
GIMG_API void gimg_jpeg_tables_destroy(GIMG_JPEG_Tables * tables);

/**
 * @brief Supply the bytes of a profile a file named rather than carried.
 *
 * @param user GIMG_Load_Options.icc_resolver_user, untouched.
 * @param path The path the file stated, NUL-terminated, as stored - not
 *   necessarily UTF-8, and not necessarily meaningful on this machine.
 * @param out_profile Receives the profile bytes, which the library copies.
 * @param out_size Receives their length.
 * @return GIMG_OK to attach the profile; anything else leaves the image
 *   untagged, which is not an error.
 */
typedef GIMG_Result (*GIMG_ICC_Resolver_Fn)(void * user, const char * path,
    const void ** out_profile, size_t * out_size);

/**
 * @brief BMP: what to make of the fourth byte of a 32-bit BI_RGB pixel.
 * @see api_options
 */
/** BMP: write an indexed bitmap when that is lossless and smaller. */
#define GIMG_BMP_PALETTE_AUTO 0
/** BMP: always write 24- or 32-bit color. */
#define GIMG_BMP_PALETTE_NEVER 1

/** BMP: write the pixel data plainly, not as a wrapped image. Default. */
#define GIMG_BMP_WRAPPER_NONE 0
/** BMP: write a `BI_JPEG` wrapper, whose pixel data is a whole JPEG. */
#define GIMG_BMP_WRAPPER_JPEG 1
/** BMP: write a `BI_PNG` wrapper, whose pixel data is a whole PNG. */
#define GIMG_BMP_WRAPPER_PNG 2

/** ICO: choose PNG or DIB per the AUTO rule (see GIMG_Save_Options.ico_payload). */
#define GIMG_ICO_PAYLOAD_AUTO 0
/** ICO: write every entry as a bare DIB (with AND mask). */
#define GIMG_ICO_PAYLOAD_DIB 1
/** ICO: write every entry as a PNG payload. */
#define GIMG_ICO_PAYLOAD_PNG 2

/** BMP: never run-length encode. */
#define GIMG_BMP_RLE_NEVER 0
/** BMP: write BI_RLE8 for an 8-bit indexed image when it comes out smaller. */
#define GIMG_BMP_RLE_AUTO 1

/** Ignore it: a 32-bit BI_RGB image decodes fully opaque.  This is the
 * default, and what GDI, Pillow, GdkPixbuf and netpbm all do. */
#define GIMG_BMP_RGB32_ALPHA_IGNORE 0
/** Read it as alpha when any pixel in the image sets it, and as opaque when
 * every one of them is zero. */
#define GIMG_BMP_RGB32_ALPHA_HEURISTIC 1

/**
 * @brief Load options (limits, strictness, etc.).
 * @see api_options
 */
typedef struct {
  const GIMG_Limits * limits; ///< NULL = use defaults.
  /**
   * @warning **Not honoured.** No codec reads this field. It is declared so
   * the shape of the struct is settled, and every codec currently behaves as
   * GIMG_NORMAL describes whatever is set here. Setting GIMG_STRICT does not
   * make anything stricter, so do not rely on it to reject a file - use the
   * result code and, on a load, GIMG_Diagnostics, which every refusal fills.
   *
   * Its zero value is GIMG_STRICT, so the usual zero-initialized options ask
   * for the strictest setting today and would change behaviour the day this
   * is implemented. That is worth knowing before writing code that depends on
   * either answer.
   */
  GIMG_Strictness strictness;
  /** Tables to install before reading the stream (T.81 B.4).
   *
   * Needed only for the abbreviated format for compressed image data - a frame
   * whose own tables are absent.  A complete JPEG carries its tables and
   * ignores this, except that its own segments override whatever these
   * installed, which is what B.2.4.1's "until redefined" means.  Ignored for
   * non-JPEG. */
  const GIMG_JPEG_Tables * jpeg_tables;
  /** BMP: what the fourth byte of a 32-bit BI_RGB pixel means.
   *
   * BI_RGB leaves that byte undefined, and writers split roughly evenly
   * between storing alpha there and storing zero.  There is no way to tell
   * the two apart from the file, so this is a policy and not a deduction.
   *
   * GIMG_BMP_RGB32_ALPHA_IGNORE (0, the default) decodes such an image fully
   * opaque, which is what every other decoder does and what the format says
   * the byte means.  GIMG_BMP_RGB32_ALPHA_HEURISTIC reads the byte as alpha
   * when any pixel in the image sets it, which recovers an alpha channel a
   * writer put there without declaring it - at the cost of making an image
   * whose spare bits are merely dirty come out full of holes.
   *
   * A file that declares its alpha - BI_BITFIELDS or BI_ALPHABITFIELDS with
   * an alpha mask, or a V3 or later header - is not affected by this: its
   * mask is honored as written either way.  Ignored for non-BMP. */
  uint8_t bmp_rgb32_alpha;
  uint8_t _reserved[7];
  /** Called when a file names an ICC profile rather than carrying one.
   *
   * BMP's PROFILE_LINKED is the case: the header holds a file path. This
   * library never opens it. Opening a path that arrived inside an image is
   * acting on data, and only the caller knows where the image came from, which
   * directories are theirs, and whether a Windows path out of a 1990s BMP
   * should be mapped onto a local profile at all.
   *
   * So the decision is handed over whole. Return GIMG_OK with the profile
   * bytes to attach them, or anything else to leave the image untagged - which
   * is also what happens when no resolver is set, and is not an error. The
   * bytes are copied before the call returns; ownership stays with the caller
   * and nothing here frees them.
   *
   * A profile larger than the codec's ceiling, or than
   * GIMG_Limits.max_memory, is refused the same way an oversized embedded one
   * is. Ignored by formats that have no such thing. */
  GIMG_ICC_Resolver_Fn icc_resolver;
  /** Passed to icc_resolver untouched. */
  void * icc_resolver_user;
} GIMG_Load_Options;

/**
 * @brief Load document from stream (stub: returns UNSUPPORTED or minimal doc).
 */
GIMG_API GIMG_Result gimg_doc_load(GIMG_Stream * stream,
    const GIMG_Load_Options * options, GIMG_Diagnostics * diagnostics,
    GIMG_Doc ** out_doc);

/** @brief EXIF IFD1 thumbnail format: 0 = default (6), 1 = uncompressed,
 * 6 = JPEG, 7 = TIFF TechNote 2 JPEG. Used when saving with a second doc item.
 */
#define GIMG_EXIF_THUMB_FORMAT_DEFAULT 0
#define GIMG_EXIF_THUMB_FORMAT_UNCOMPRESSED 1
#define GIMG_EXIF_THUMB_FORMAT_JPEG 6
#define GIMG_EXIF_THUMB_FORMAT_TIFF_JPEG 7

/** @brief JPEG chroma subsampling: 0 = 4:2:0 (default), 1 = 4:2:2, 2 = 4:4:4. */
#define GIMG_JPEG_CHROMA_420 0
#define GIMG_JPEG_CHROMA_422 1
#define GIMG_JPEG_CHROMA_444 2
/** FDCT method: Loeffler (libjpeg-compatible, default) or the T.81 A.3.3
 * definition, which is there as an oracle for the first; see
 * GIMG_Save_Options. */
#define GIMG_JPEG_FDCT_LOEFFLER 0
#define GIMG_JPEG_FDCT_REF     1
/** Quantization method: reciprocal-based (libjpeg-compatible, default) or
 * integer division.  The two write the same file; see GIMG_Save_Options. */
#define GIMG_JPEG_QUANT_RECIP  0
#define GIMG_JPEG_QUANT_DIV    1

/**
 * @brief One scan in a progressive JPEG scan script (ISO/IEC 10918-1 Annex B).
 * Ss, Se = spectral selection (coefficient indices 0–63, zigzag order).
 * Ah, Al = successive approximation (Ah=0 for initial pass; refinement when Ah>0).
 */
typedef struct {
  uint8_t Ss; ///< First coefficient index in this scan (0–63).
  uint8_t Se; ///< Last coefficient index in this scan (0–63; must be >= Ss).
  uint8_t Ah; ///< Successive approximation high (0 = initial encoding).
  uint8_t Al; ///< Successive approximation low / bit position.
} GIMG_JPEG_Progressive_Scan;

/**
 * @brief Progressive JPEG scan script. When saving with jpeg_progressive=1:
 * - If NULL or scan_count==0: encoder uses default progression (e.g. DC + AC bands).
 * - If non-NULL and scan_count>0: encoder uses this sequence of scans.
 * Caller keeps the array valid for the duration of gimg_doc_save().
 */
typedef struct {
  unsigned int scan_count;
  const GIMG_JPEG_Progressive_Scan * scans;
} GIMG_JPEG_Progressive_Config;

/**
 * @brief Save options (metadata policy, interlace, quality, etc.).
 * @see api_options
 */
/**
 * @brief Save options.
 *
 * **Every field that only one format reads carries that format's name.** The
 * struct is flat and shared, so `png_interlaced` and `gif_interlace` sit a
 * few lines apart and mean different things; without the prefix there is
 * nothing at the point of use to say which writer will read what you set. A
 * save case in this library's own test suite set the unprefixed `interlaced`
 * expecting the GIF writer to read it, produced a byte-for-byte copy of the
 * plain case, and passed.
 *
 * A field that more than one codec reads has no prefix - `metadata_policy`,
 * and the `exif_thumbnail_*` pair, which JPEG and PNG both honour.
 *
 * @see api_options
 */
typedef struct {
  GIMG_Meta_Policy metadata_policy;
  /** PNG: 0 = non-interlaced (default), 1 = Adam7 (11.2.2). Ignored for
   * non-PNG; GIF's four-pass order is gif_interlace, which is a different
   * field for a different format. */
  unsigned int png_interlaced;
  /** JPEG quality 1–100 (100 = finest). 0 = unspecified, codec default
   * (85). Ignored for non-JPEG. */
  unsigned int jpeg_quality;
  uint8_t exif_thumbnail_format;  ///< IFD1 thumbnail: 0 = default (6), 1, 6, 7.
  uint8_t exif_thumbnail_quality; ///< Thumbnail JPEG quality 1–100 when
                                  ///< format 6 or 7; 0 = default (85).
  uint8_t jpeg_chroma_subsampling; ///< GIMG_JPEG_CHROMA_420 (default), 422, 444.
  /** Which forward DCT to use.
   *
   * GIMG_JPEG_FDCT_LOEFFLER (0, default) is the factored integer transform
   * every encode should want: the coefficients it produces are byte-exact
   * against libjpeg-turbo.
   *
   * GIMG_JPEG_FDCT_REF is T.81 A.3.3 equation 4 evaluated directly in double
   * precision - sixty-four multiply-adds per coefficient instead of a handful.
   * It exists to be an oracle rather than a choice: being the definition, it
   * has no factoring to get wrong, so differencing the two says whether the
   * fast one is right without libjpeg installed.  Over 20000 random blocks the
   * two never differ by more than one in any coefficient and agree exactly at
   * DC.
   *
   * **Expect it to be slow.**  Use it to check a build, or on a platform where
   * the fast transform is suspect; do not reach for it to make prettier files,
   * because it does not - the difference is under one part in a quantization
   * step. */
  uint8_t jpeg_fdct_method;
  /** Quantization: GIMG_JPEG_QUANT_RECIP (0, default) or
   * GIMG_JPEG_QUANT_DIV (1).
   *
   * **This chooses how the division is done, not what it produces.**  The
   * reciprocal form is an exact division by multiplication rather than an
   * approximation of one, so both settings quantize every coefficient to the
   * same value and write byte-identical files.  Measured over 204
   * combinations of quality, progression and subsampling; see
   * JpegEncode.TheFdctAndQuantizationMethodOptionsChangeNoOutput.  Pick it
   * for speed if a measurement says to, not for quality. */
  uint8_t jpeg_quant_method;
  uint8_t jpeg_progressive;        ///< 0 = baseline (default), 1 = progressive.
  /** When jpeg_progressive==1: NULL or scan_count 0 = default progression;
   * otherwise use this scan script. Ignored for non-JPEG or baseline. */
  const GIMG_JPEG_Progressive_Config * jpeg_progressive_config;
  /** Restart interval in MCUs (0 = none). When non-zero, DRI segment is
   * written and RST markers (0xFF 0xD0..0xD7) are injected every N MCUs. */
  uint16_t jpeg_restart_interval;
  /** JPEG output precision (save): 0 = derive from the raster, 8 or 12 = write
   * at that precision.  T.81 Table B.2 allows only 8 and 12 in a DCT-based
   * frame, so 16 returns GIMG_ERR_UNSUPPORTED and a 16-bit raster is written
   * at 12 when this is 0.  When raster depth differs from the chosen
   * precision, the encoder uses library bit-depth conversion
   * (gimg_ops_convert_bit_depth or gimg_bitdepth_*). Ignored for non-JPEG. */
  uint8_t jpeg_precision;
  /** Write the frame with arithmetic entropy coding (T.81 Annex D) rather than
   * Huffman (Annex F): SOF9 for a sequential frame, SOF10 for a progressive
   * one, with a DAC segment and no DHT.  Both coders are normative parts of
   * T.81 and produce equally valid JPEG; arithmetic is typically a few per cent
   * smaller and is understood by far fewer decoders, so Huffman remains the
   * default.  Ignored for non-JPEG. */
  uint8_t jpeg_arithmetic;
  /** Write a lossless frame (T.81 Annex H, SOF3) instead of a DCT-based one,
   * using this predictor selection value.  0 (default) writes a DCT frame; 1
   * to 7 select a predictor from Table H.1 - 1 is the sample to the left, 2 the
   * one above, and 4 to 7 combine them.  The reconstruction is exact, so
   * `quality` and `jpeg_chroma_subsampling` have no meaning and are ignored,
   * and color is stored as RGB rather than YCbCr because that conversion is
   * not reversible.  Precision follows the raster: 8-bit rasters give P=8,
   * 12-bit P=12, 16-bit P=16, all of which Table B.2 permits in a lossless
   * frame.  Ignored for non-JPEG. */
  uint8_t jpeg_lossless_predictor;
  /** Write the image as a hierarchical sequence of frames (T.81 Annex J)
   * rather than as one frame, with this many resolution doublings.  0
   * (default) writes a single frame; 1 writes a half-size frame followed by a
   * differential frame that restores full size, 2 a quarter-size frame and two
   * differential frames, and so on.
   *
   * A hierarchical file decodes to a picture of the same size and much the
   * same quality as an ordinary one: what it buys is that a decoder can stop
   * early and still have a smaller complete image, which is what
   * multi-resolution environments want.
   *
   * A DCT sequence pays for that in size - the pyramid's lower levels are
   * extra data - and on a 64x48 test image a one-level sequence came out
   * about 11% larger than the same image written flat, a two-level one 26%.
   * A **lossless** sequence can go either way, because there the pyramid
   * replaces the flat predictor rather than adding to it: on a smooth image
   * the differential frames coded 18% smaller than a flat lossless file, and
   * on noise 5% larger.  This comment used to say "and is larger" without
   * qualification, which was never true of the lossless case.  Sampling is 4:4:4 throughout -
   * the pyramid is already doing the scaling - so `jpeg_chroma_subsampling` is
   * ignored, and the raster must be 8-bit.  Combines with `jpeg_arithmetic`,
   * which selects SOF9 and SOF13 in place of SOF1 and SOF5.  Ignored for
   * non-JPEG. */
  uint8_t jpeg_hierarchical_levels;

  /** Write a sequential frame as one non-interleaved scan per component
   * (T.81 A.2.3) rather than as a single interleaved scan (A.2.2).
   *
   * Both orders describe the same blocks and decode to the same picture; what
   * differs is the order they are written in and, with it, which decoders and
   * which pipelines can work on one component at a time.  A decoder that wants
   * only the luminance of a color image can stop after the first scan.
   *
   * A single-component image is already non-interleaved by definition, so the
   * option changes nothing there.  It combines with `jpeg_arithmetic` and with
   * `jpeg_restart_interval` - the restart interval then counts single blocks,
   * because that is what an MCU is in a non-interleaved scan (A.2.3).  It is
   * refused together with `jpeg_progressive` (Annex G has its own scan script,
   * and its AC scans are non-interleaved already), with a lossless frame, and
   * with `jpeg_hierarchical_levels`.  Ignored for non-JPEG. */
  uint8_t jpeg_non_interleaved;

  /** Adobe APP14 color transform for a four-component (CMYK) raster.
   *
   * T.81 describes no color space at all: a frame has Nf components and
   * nothing says what they mean.  For four components the convention is
   * Adobe's APP14 marker, and it is the only thing in the file that
   * distinguishes the two readings - which is why this codec writes that
   * marker for every four-component frame, and why a decoder (libjpeg's
   * jdapimin.c included) has nothing to go on without it.
   *
   * 0 (the default) writes the four components unchanged, as CMYK.  A raster
   * that came from a CMYK JPEG therefore survives a load and save unchanged.
   * 2 writes YCCK: the first three components become the YCbCr of the
   * complement of C, M and Y, which is more compressible, and K is untouched.
   * Only 0 and 2 are accepted; anything else returns GIMG_ERR_UNSUPPORTED.
   * Ignored unless the raster is GIMG_PIXEL_CMYK8, and for non-JPEG.
   *
   * Chroma subsampling applies to a YCCK frame's two chrominance components
   * and to nothing else: a raw CMYK frame has no chrominance, and libjpeg does
   * not subsample one either. */
  uint8_t jpeg_cmyk_transform;

  /** Write one of the abbreviated formats of T.81 B.4 rather than a complete
   * JPEG.
   *
   * 0 (the default) writes a complete file: tables and frame together.
   *
   * 1 writes the abbreviated format for compressed image data - the frame with
   * its table-specification segments left out.  Such a file is not a JPEG on
   * its own; it is read by passing the matching tables in
   * GIMG_Load_Options.jpeg_tables.  The tables it needs are whatever the same
   * options would have written, so a tables stream and an image stream saved
   * with the same quality, precision and entropy coder belong together.
   *
   * 2 writes the abbreviated format for table-specification data - the tables
   * alone, between SOI and EOI, with no frame.  The raster is read only for
   * its shape: nothing of the picture reaches such a file.
   *
   * Refused together with `jpeg_hierarchical_levels`, whose frames carry their
   * own tables (B.3.1), and with a lossless frame, whose Huffman table is
   * generated from the very coefficients it codes and so cannot be written
   * before them.  Ignored for non-JPEG. */
  uint8_t jpeg_abbreviated;
  /** @name Arithmetic conditioning (T.81 B.2.4.3, the DAC segment)
   *
   * What the arithmetic coder treats as a small difference and where it stops
   * counting a block's coefficients as low-frequency.  These only reach a file
   * written with `jpeg_arithmetic`; a Huffman frame has no DAC segment and
   * ignores them.
   *
   * They are a tuning choice, not a correctness one: any conforming decoder
   * reads the DAC and follows it, and the same picture comes back whatever is
   * set.  What changes is how well the coder's statistics fit the image, and so
   * the size of the file.  B.2.4.3's defaults suit photographic data; an image
   * with an unusually flat or unusually busy DC channel does better elsewhere.
   *
   * L greater than U is refused with GIMG_ERR_UNSUPPORTED, as is any value out
   * of the range its clause gives, rather than being clamped: a caller who
   * asked for conditioning a decoder would reject should hear about it here
   * and not from the decoder.
   * @{ */
  /** DC conditioning lower bound L, 0 to 15.  0 is both the default and a
   * legal value, so there is no sentinel to distinguish. */
  uint8_t jpeg_arith_dc_l;
  /** DC conditioning upper bound U, L to 15.  0 means B.2.4.3's default of 1,
   * which is why U cannot be set to 0 - a band from 0 to 0 is what L = U = 0
   * already says. */
  uint8_t jpeg_arith_dc_u;
  /** AC conditioning Kx, 1 to 63.  0 means B.2.4.3's default of 5. */
  uint8_t jpeg_arith_ac_k;
  /** @} */
  /** PNG row filter (11.2.4, filter method 0). GIMG_PNG_FILTER_ADAPTIVE (0,
   * default) chooses per row by the heuristic PNG 12.8 recommends; the other
   * values force one filter on every row, which is mainly useful for testing
   * that each of the five reconstructs. */
  uint8_t png_filter;
  /** How the PNG writer decides whether to store the image through a palette
   * (11.2.2, color type 3).
   *
   * The three values answer one question, which is worth naming because the
   * field looks at first like it answers two: **what happens when the palette
   * the frame arrived with no longer describes the picture.** Build a new one
   * (AUTO), write truecolor (NEVER), or refuse (KEEP). Those are the only
   * three outcomes there are. While an inherited palette still fits, all three
   * reuse it, so "preserve what arrived" is not a separate axis - it is what
   * every value already does.
   *
   * GIMG_PNG_PALETTE_AUTO (0, default) builds one when the image has no more
   * than 256 distinct colors and the palette form is the smaller file.  That
   * is a lossless choice and not color quantization: with 256 colors or
   * fewer there is exactly one palette that reproduces the image, so nothing
   * is decided about the picture - only about how it is stored.  An image with
   * more colors than that is written as truecolor, because reducing it would
   * be an image-processing decision and not a codec's.
   *
   * GIMG_PNG_PALETTE_NEVER refuses to build one, and writes truecolor.
   *
   * A frame that *arrived* as a palette image is written back through its own
   * palette under both of those, which preserves the entries and their order
   * rather than creating something new - but only while that palette still
   * describes the picture.  It need not: one pixel painted a color the table
   * does not hold is enough, and an APNG needs no edit at all, because a frame
   * is composited onto the canvas before it becomes a raster and compositing a
   * partly transparent entry produces a color that is in no palette.  When the
   * palette no longer fits, AUTO builds one that does and NEVER writes
   * truecolor; both are exact, so nothing about the picture is lost - only the
   * indices.
   *
   * GIMG_PNG_PALETTE_KEEP is for the caller who means those indices: it treats
   * the palette the file arrived with as a constraint and returns
   * GIMG_ERR_UNSUPPORTED, before writing anything, for an image that has left
   * it.  With no palette to keep it behaves as AUTO.  Ignored for non-PNG. */
  uint8_t png_palette;

  /** Whether the BMP writer may store an image through a palette.
   *
   * GIMG_BMP_PALETTE_AUTO (0, default) writes an indexed bitmap when the
   * image has no more than 256 distinct colors, is fully opaque, and the
   * indexed form is the smaller file - at the smallest depth that holds the
   * indices, 1, 4 or 8 bits.  Like the PNG writer's palette, that is a
   * lossless choice and not color quantization: with 256 colors or fewer
   * there is exactly one palette that reproduces the image, so nothing is
   * being decided about the picture, only about how it is stored.  Unlike
   * PNG's, the two sizes are arithmetic rather than a measurement, so both
   * forms need not be written to find out which is smaller.
   *
   * GIMG_BMP_PALETTE_NEVER writes 24- or 32-bit color always.  An image with
   * more than 256 colors, or with any transparency, is written that way
   * regardless: a BMP palette has no alpha, and reducing the colors would be
   * an image-processing decision and not a codec's.  Ignored for non-BMP. */
  uint8_t bmp_palette;

  /** Whether the BMP writer may run-length encode an indexed bitmap.
   *
   * GIMG_BMP_RLE_NEVER (0, default) writes the rows uncompressed.  That is
   * the default because an uncompressed BMP is the most widely readable image
   * there is, which is most of why the format is still worth writing;
   * `bmptopnm` refuses several of bmpsuite's RLE files, to name one reader in
   * reach of this repository.
   *
   * GIMG_BMP_RLE_AUTO writes BI_RLE8 or BI_RLE4 - whichever matches the depth
   * the image is being stored at - when the encoded rows come out smaller than
   * the plain ones.  Both are measured rather than assumed: RLE4's alternating
   * nibbles make it larger than RLE4's plain rows on most images that are not
   * synthetic, so it is written only where it actually wins.  RLE24 is not
   * reached from here; it needs bmp_allow_rle24, because it can only be
   * written in an OS/2 header.  Ignored for non-BMP. */
  uint8_t bmp_rle;

  /** Whether the BMP writer may store an indexed image at 2 bits per pixel.
   *
   * 0 (the default) uses 1, 4 or 8 bits, which is what the desktop Windows
   * API accepts.  2 bits per pixel is a Windows CE addition: it is read by
   * this codec and by little else, and an image that fits in four colors fits
   * in 1 or 4 bits as well, so writing one trades reach for a few bytes.
   *
   * 1 lets the depth chooser use it when the palette has three or four
   * entries.  A caller who knows what will read the file can have those bytes;
   * nobody gets them by accident.  Ignored for non-BMP. */
  uint8_t bmp_allow_2bit;

  /** Whether the BMP writer may use the OS/2 RLE24 encoding.
   *
   * 0 (the default) never writes it.  RLE24 exists only in an OS/2
   * BITMAPCOREHEADER2, where compression 4 means RLE24 and Windows reads the
   * same number as BI_JPEG - so a file carrying it is not a Windows BMP at
   * all, and handing one to a Windows reader is worse than handing it
   * something merely uncompressed.
   *
   * 1 lets a true-color image be written as an OS/2 bitmap with RLE24 when
   * the encoded rows come out smaller.  The header changes vocabulary with
   * it; that is not a side effect but the whole of what this option means.
   * Requires bmp_rle to be GIMG_BMP_RLE_AUTO as well: this says which
   * encodings are permitted, not that anything should be compressed.
   * Ignored for non-BMP. */
  uint8_t bmp_allow_rle24;

  /** Whether the BMP writer may use the OS/2 Huffman 1D encoding.
   *
   * 0 (the default) never writes it.  Like RLE24 it lives only in an OS/2
   * BITMAPCOREHEADER2 - compression 3 there is CCITT Group 3 one-dimensional
   * coding, where a Windows reader sees BI_BITFIELDS - so a file carrying it
   * is not a Windows BMP, and of the decoders reachable from here only bmplib
   * reads one.
   *
   * 1 lets a two-colour image be written that way when the encoded stream
   * comes out smaller than the packed rows, which for large flat areas it
   * comfortably is.  Requires bmp_rle to be GIMG_BMP_RLE_AUTO as well: this
   * says which encodings are permitted, not that anything should be
   * compressed.  Ignored for non-BMP. */
  uint8_t bmp_allow_huffman;

  /** Whether the BMP writer wraps a whole JPEG or PNG as the pixel data.
   *
   * GIMG_BMP_WRAPPER_NONE (0, the default) writes pixels.  The wrapper forms
   * exist so a BMP can carry an already-compressed image, and were meant for
   * spooling to printers rather than for interchange: most readers refuse
   * them, and every decoder reachable from here except this one does.  A
   * caller who wants a JPEG can save a JPEG, which is why nothing reaches for
   * these on its own.
   *
   * GIMG_BMP_WRAPPER_JPEG and GIMG_BMP_WRAPPER_PNG write that format's stream
   * as the pixel data, with biCompression saying which.  The payload is
   * produced by this library's own codec for that format, so its own save
   * options do not reach it - a wrapper is a container choice, and the picture
   * inside it is written at that codec's defaults.  Ignored for non-BMP. */
  uint8_t bmp_wrapper;

  /** Whether the BMP writer stores rows top to bottom.
   *
   * 0 (the default) writes them bottom-up with a positive biHeight, which is
   * the layout every reader handles.  1 writes them top-down with a negative
   * biHeight, which is legal from BITMAPINFOHEADER onwards and is what a
   * caller wants when something downstream reads the file as a memory-mapped
   * framebuffer.  It is refused together with GIMG_BMP_RLE_AUTO, because the
   * format does not allow compression and top-down rows together - an RLE
   * stream's end-of-line walks one way only.  Ignored for non-BMP. */
  uint8_t bmp_top_down;

  /** Whether the GIF writer stores rows in the four-pass interlaced order
   * (89a 20).
   *
   * 0 (the default) writes them top to bottom.  1 interlaces them, which lets
   * a reader show a coarse version of the picture before the whole file has
   * arrived - the reason the format has it, and worth little now that files
   * arrive faster than they are looked at.  It changes no pixel either way.
   * Ignored for non-GIF. */
  uint8_t gif_interlace;

  /** What the GIF writer does with a pixel that is neither fully opaque nor
   * fully transparent.
   *
   * GIF has one bit of transparency: a single palette index is designated
   * transparent and every other pixel is opaque (89a 23).  There is no way to
   * store a half-covered edge, so an alpha of 128 cannot be written, only
   * decided about - and which way to decide is the caller's business, not the
   * codec's.
   *
   * 0 (the default) refuses such a raster with GIMG_ERR_UNSUPPORTED rather
   * than choosing silently.  A value of 1 to 255 is a threshold: alpha at or
   * above it becomes opaque, below it becomes the transparent index.  128 is
   * the usual choice.  Fully opaque and fully transparent pixels are written
   * the same way whatever this says.  Ignored for non-GIF. */
  uint16_t gif_alpha_threshold;

  /** How many times a written GIF animation repeats, overriding the document.
   *
   * Written as the NETSCAPE2.0 Application Extension that every decoder reads
   * for this (89a 26 describes the block; the loop count inside it is a
   * convention, not part of the specification).
   *
   * **This is an override, not the source.** The count normally comes from
   * gimg_doc_set_loop_count(), so a loaded animation keeps the count it
   * declared without the caller carrying it across.  A non-zero value here
   * wins; zero - the default - means the caller did not ask and the document
   * answers.  Zero cannot mean "repeat forever" here because it cannot also
   * mean "not set"; say that on the document instead, which has a spelling for
   * both (a count of 0, or no count at all).
   *
   * A document declaring no count at all gets no NETSCAPE2.0 block, which is a
   * different instruction from a count of zero: browsers play such a file
   * once.  Ignored for non-GIF. */
  uint16_t gif_loop_count;

  /** TIFF: how the strips are compressed.
   *
   * GIMG_TIFF_COMPRESS_NONE (0, the default) stores them, which is what the
   * format's own default is and what a caller who chose TIFF for an
   * uncompressed archive master expects. The other three are one option
   * away and all lossless: PackBits is the format's own trivial run-length
   * coding, LZW is what most TIFF writers use, and Deflate is the smallest.
   * Ignored for non-TIFF. */
  uint8_t tiff_compression;
  /** TIFF: horizontal differencing before compression (Predictor, 317).
   *
   * 0 and 1 both mean none, which is the default. 2 subtracts each sample
   * from the one a pixel to its left, which costs nothing and usually
   * shrinks a photograph markedly under LZW or Deflate.
   *
   * Refused with GIMG_ERR_UNSUPPORTED when tiff_compression is NONE - a
   * predictor with no compressor behind it makes a file larger and harder to
   * read for nothing - and at a bit depth other than 8 or 16, which is where
   * TIFF defines it. Ignored for non-TIFF. */
  uint8_t tiff_predictor;
  /** TIFF: write the file big-endian ("MM") rather than little ("II").
   *
   * Both are the format, equally, and a reader that cannot take either is
   * broken; this exists because saying so is cheap and because a round trip
   * through both orders is the sharpest test a byte-order-agnostic reader
   * has. 0, the default, writes "II". Ignored for non-TIFF. */
  uint8_t tiff_big_endian;
  uint8_t _reserved_tiff;
  /** TIFF: rows in one strip. 0, the default, picks a number that puts about
   * eight kilobytes in each - libtiff's own rule, and for the same reason: a
   * strip is the unit a reader has to hold at once. Ignored for non-TIFF. */
  uint32_t tiff_rows_per_strip;

  /** How the ICO/CUR writer encodes each entry's payload.
   *
   * GIMG_ICO_PAYLOAD_AUTO (0, default) writes a PNG when either dimension is
   * greater than 128 or the source has non-trivial alpha, and a DIB otherwise.
   * GIMG_ICO_PAYLOAD_DIB and GIMG_ICO_PAYLOAD_PNG force one kind for every
   * entry. Ignored for non-ICO. */
  uint8_t ico_payload;

  /** WebP: which compressor. GIMG_WEBP_COMPRESS_LOSSLESS (0, default / AUTO)
   * writes VP8L. GIMG_WEBP_COMPRESS_LOSSY writes a VP8 keyframe, and a
   * non-opaque picture also writes an ALPH chunk. Ignored for non-WebP. */
  uint8_t webp_lossless;

  /** WebP: encoder effort, 0–9. Lossless: higher may apply subtract-green
   * (effort >= 1) and the later VP8L passes. Lossy: selects the quantizer,
   * coarser at 0. Effort 4 is index 26, the single-segment index cwebp
   * writes at quality 75. Default when options is NULL is 4; a present
   * options struct with this field left 0 means effort 0. Ignored for
   * non-WebP. */
  uint8_t webp_effort;

  /** WebP: non-zero preserves RGB samples under fully transparent pixels
   * (libwebp `-exact`). Zero (default) clears them, which usually shrinks
   * the file. Ignored for non-WebP. */
  uint8_t webp_exact;

  /** Zero; room to grow. */
  uint8_t _reserved[4];
} GIMG_Save_Options;

/** @name WebP compressor choice
 * @{ */
#define GIMG_WEBP_COMPRESS_LOSSLESS 0u ///< VP8L (default / AUTO).
#define GIMG_WEBP_COMPRESS_LOSSY 1u    ///< VP8 keyframe. Non-opaque input adds ALPH.
/** @} */

/** @name TIFF compression (TIFF 6.0 sections 9 and 13)
 *
 * Every one of these is lossless; the format's lossy option is JPEG, which
 * this codec neither reads nor writes.
 * @{ */
#define GIMG_TIFF_COMPRESS_NONE 0u     ///< Store the strips. The default.
#define GIMG_TIFF_COMPRESS_PACKBITS 1u ///< PackBits (section 9).
#define GIMG_TIFF_COMPRESS_LZW 2u      ///< LZW (section 13).
#define GIMG_TIFF_COMPRESS_DEFLATE 3u  ///< Deflate, the smallest of the four.
/** @} */

/** @name PNG row filters (PNG 9.2, Table 9.1)
 * @{ */
#define GIMG_PNG_FILTER_ADAPTIVE 0u ///< Choose per row (PNG 12.8). Default.
#define GIMG_PNG_FILTER_NONE 1u     ///< Filter type 0 on every row.
#define GIMG_PNG_FILTER_SUB 2u      ///< Filter type 1 on every row.
#define GIMG_PNG_FILTER_UP 3u       ///< Filter type 2 on every row.
#define GIMG_PNG_FILTER_AVERAGE 4u  ///< Filter type 3 on every row.
#define GIMG_PNG_FILTER_PAETH 5u    ///< Filter type 4 on every row.
/** @} */

/** @name PNG palette creation (PNG 11.2.2, color type 3)
 * @{ */
#define GIMG_PNG_PALETTE_AUTO 0u  ///< Build one when it is lossless and smaller. Default.
#define GIMG_PNG_PALETTE_NEVER 1u ///< Never build one for a raster that had none.
#define GIMG_PNG_PALETTE_KEEP 2u  ///< Refuse an image that has left the palette it arrived with.
/** @} */

/**
 * @brief Save report (warnings, bytes written, etc.).
 */
typedef struct {
  size_t bytes_written; ///< Bytes the save actually wrote; always filled.
  /**
   * @warning **Never set.** No codec's save path writes to this pointer, so
   * it holds whatever the caller left in it. Saving reports through its
   * result code alone.
   *
   * Of the three, only gimg_doc_load() fills diagnostics: it takes a
   * GIMG_Diagnostics and every refusal in every codec appends an item saying
   * which rule was broken. gimg_item_decode() has no diagnostics parameter,
   * so a decode reports through its result code alone as well.
   */
  GIMG_Diagnostics * diagnostics;
  uint8_t _reserved[8];
} GIMG_Save_Report;

/**
 * @brief Save a document to a stream in the named format.
 *
 * @param doc The document. Its item 0 supplies the pixels: the raster
 *   attached to it when there is one, otherwise the writer decodes the item
 *   itself, whichever codec loaded the document.
 *
 *   What becomes of the items after it is the format's to say, and they do
 *   not agree. PNG writes them as the frames of an APNG, GIF as the frames
 *   of an animation, and WebP as ANMF frames of the rectangle that
 *   changed; JPEG writes item 1
 *   as the Exif thumbnail and ignores the
 *   rest; BMP writes item 0 alone and returns GIMG_OK, because the usual way
 *   to ask for one frame of an animation as a BMP is to hand the whole
 *   animation over, and refusing that would be worse than dropping the frames
 *   the format cannot hold. So saving a document of several items as a BMP is
 *   lossy by design and says so only here.
 * @param stream Destination, opened for output.
 * @param format_name "png", "jpeg", "bmp" or "gif" - the name of a
 *   registered codec, as gimg_codec_name() reports it.
 * @param options May be NULL for the defaults; see @ref api_options.
 * @param report Required. On success its bytes_written is the file size.
 * @return GIMG_OK; GIMG_ERR_UNSUPPORTED when the raster's pixel format is one
 *   the named codec cannot write, or when no codec has that name;
 *   GIMG_ERR_CORRUPT when @p options asks for a metadata policy that edits
 *   Exif (GIMG_META_STRIP_GPS, GIMG_META_NORMALIZE_EXIF) and the document's
 *   Exif cannot be parsed - the save reports rather than silently leaving the
 *   policy unapplied;
 *   GIMG_ERR_INTERNAL for a null argument, @p report included - it is
 *   required, unlike @p options.
 */
GIMG_API GIMG_Result gimg_doc_save(const GIMG_Doc * doc, GIMG_Stream * stream,
    const char * format_name, const GIMG_Save_Options * options,
    GIMG_Save_Report * report);

/**
 * @brief JPEG chroma upsampling method (decode only).
 * @see api_options
 */
/** Codec default, which is FANCY.  Zero so that a zero-initialized
 * GIMG_Decode_Options decodes exactly as a NULL one does - see below. */
#define GIMG_JPEG_CHROMA_UPSAMPLE_DEFAULT 0
#define GIMG_JPEG_CHROMA_UPSAMPLE_FANCY  1  /**< Triangle filter (smooth). */
#define GIMG_JPEG_CHROMA_UPSAMPLE_SIMPLE  2  /**< Box filter (replicate). */

/** @name GIF background colour (89a 18 and 23)
 *
 * What the decoder puts where no frame has drawn: the screen before the first
 * frame, and the rectangle disposal method 2 asks to have cleared.
 *
 * The two values are "what a browser shows" and "what the specification says".
 * They are different, and which one a caller wants depends on what they are
 * building rather than on which is correct.  See \ref format_gif "GIF" -
 * "The background colour" - for the measurements behind that, including why
 * neither setting makes this decoder agree with ImageMagick or Pillow.
 * @{ */
/** Leave it transparent.  The default, and what browsers do. */
#define GIMG_GIF_BACKGROUND_TRANSPARENT 0u
/** Paint the colour the Background Color Index names, as 89a 18 and 23 say. */
#define GIMG_GIF_BACKGROUND_PAINT 1u
/** @} */

/**
 * @brief Decode options.
 * @see api_options
 */
typedef struct {
  const GIMG_Limits * limits;
  /** JPEG: chroma upsampling when decoding 4:2:0/4:2:2.
   * GIMG_JPEG_CHROMA_UPSAMPLE_DEFAULT (0), FANCY (1) or SIMPLE (2).
   * DEFAULT means FANCY, so passing a zero-initialized GIMG_Decode_Options
   * and passing NULL select the same filter.  SIMPLE deliberately does not
   * live at zero: when it did, `GIMG_Decode_Options o = {};` quietly decoded
   * with a different filter than passing no options at all. */
  uint8_t jpeg_chroma_upsampling;
  /** JPEG decode-to precision: the depth the raster comes back at.
   *
   * 0 (default) is the file's own precision - 8 bits comes back as GRAY8 or
   * RGBA8, and 12 or 16 as GRAY12/GRAY16 or RGBA12/RGBA16.  8, 12 or 16 names
   * a depth to restate it at instead, through the same conversion
   * gimg_ops_convert_bit_depth performs, so this option saves a step rather
   * than reaching anything a caller could not reach itself.
   *
   * This is the reading counterpart of GIMG_Save_Options::jpeg_precision, but
   * it is not bounded the same way.  T.81 Table B.2 allows only 8 and 12 in a
   * DCT-based *frame*, which is why the writer refuses 16 there; that
   * constrains what a file may say and not what a caller may ask for, and
   * widening an 8-bit image to 16 is exact.  So all three are accepted here.
   *
   * Any other value is refused with GIMG_ERR_UNSUPPORTED.  It is not ignored:
   * ignoring it is what this field did before it was implemented, and a caller
   * who writes 10 has a misunderstanding that silence does not fix.
   *
   * Applies to every JPEG process - sequential, progressive, lossless and
   * hierarchical - and to the Exif thumbnail at item 1, so that one document
   * does not hand back two different depths.  Ignored for non-JPEG. */
  uint8_t jpeg_precision;
  /** GIF: what goes where no frame has drawn.
   *
   * GIMG_GIF_BACKGROUND_TRANSPARENT (0, default) leaves the logical screen
   * empty and clears disposal method 2 to transparent, which is what every
   * browser does and what the animations in the wild were authored against.
   *
   * GIMG_GIF_BACKGROUND_PAINT paints the colour the Background Color Index
   * names (89a 18), and restores that colour for disposal method 2 (89a 23) -
   * the literal reading of the specification.  It has no effect on a file with
   * no Global Color Table, where 89a 18 says the index is to be ignored.
   *
   * Painting it is lossy in one direction: it makes every such GIF opaque, and
   * a caller who wanted the alpha cannot recover it afterwards.  That is why
   * the default is the other one - the colour is reported by
   * gimg_doc_background_color() whichever is set, so compositing over it later
   * is always available.
   *
   * **Turning it on does not make this decoder match another one.**  Of the
   * decoders measured, ImageMagick honours 89a 18 but not 89a 23 and Pillow
   * the reverse; neither does both, so neither setting reproduces either of
   * them exactly.  \ref format_gif "GIF" has the table. */
  uint8_t gif_background;
  uint8_t _reserved[5];
} GIMG_Decode_Options;

/**
 * @brief Decode an item to a raster.
 *
 * Reports through its result code alone: there is no GIMG_Diagnostics
 * parameter here, so nothing a decode refuses can say which rule it broke.
 * A load can and does - see GIMG_Save_Report.diagnostics for the three-way
 * summary - so where a file is going to be rejected for its structure, it is
 * rejected at load time with a reason attached.
 */
GIMG_API GIMG_Result gimg_item_decode(const GIMG_Item * item,
    const GIMG_Decode_Options * options, GIMG_Raster ** out_raster);

/**
 * @brief Ensure the item has an attached raster: if already present, no-op;
 * otherwise decode via the document's codec and attach the raster to the item.
 * The document owns the attached raster. Use for load -> modify -> save flows.
 * @param item Item (must belong to a document that was loaded with a codec).
 * @param options Decode options (limits, etc.); NULL for defaults.
 * @return GIMG_OK, GIMG_ERR_UNSUPPORTED (no codec / not loaded), or decode
 * error.
 */
GIMG_API GIMG_Result gimg_item_ensure_decoded(
    GIMG_Item * item, const GIMG_Decode_Options * options);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GIMG_CODEC_H

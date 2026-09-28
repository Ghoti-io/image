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
 * Internal BMP codec structures: DIB header fields, channel masks, and the
 * document state carried from load to decode.
 */

#ifndef GHOTI_IO_GIMG_SRC_CODEC_BMP_BMP_INTERNAL_H
#define GHOTI_IO_GIMG_SRC_CODEC_BMP_BMP_INTERNAL_H

#include <ghoti.io/image/macros.h>

#include <ghoti.io/image/allocator.h>
#include <ghoti.io/image/codec.h>
#include <ghoti.io/image/color.h>
#include <ghoti.io/image/core.h>
#include <ghoti.io/image/doc.h>
#include <ghoti.io/image/raster.h>
#include <ghoti.io/image/stream.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** BMP file signature length (the "BM" magic). */
#define GIMG_BMP_SIGNATURE_LEN 2

/** BMP file signature bytes ('B', 'M'). */
extern const unsigned char gimg_bmp_signature[GIMG_BMP_SIGNATURE_LEN];

/** OS/2 bitmap array signature bytes ('B', 'A'). */
extern const unsigned char gimg_bmp_array_signature[GIMG_BMP_SIGNATURE_LEN];

/**
 * Size of a BITMAPARRAYFILEHEADER: the 'BA' magic, cbSize, offNext, and the
 * two display dimensions.  An ordinary BITMAPFILEHEADER follows immediately
 * after, at this offset rather than at cbSize - bmpsuite's x/ba-bm.bmp writes
 * a cbSize of 40 for a structure that is 14 bytes long, so cbSize joins bfSize
 * and biSizeImage among the fields this codec does not trust.
 */
#define GIMG_BMP_ARRAY_HEADER_SIZE 14

/** Size of the BITMAPFILEHEADER on disk. */
#define GIMG_BMP_FILE_HEADER_SIZE 14

/** DIB header sizes we recognize. */
#define GIMG_BMP_COREHEADER_SIZE 12  ///< BITMAPCOREHEADER (OS/2 1.x).
#define GIMG_BMP_INFOHEADER_SIZE 40  ///< BITMAPINFOHEADER (Windows 3.x).
#define GIMG_BMP_V2HEADER_SIZE 52    ///< BITMAPV2INFOHEADER (RGB masks).
#define GIMG_BMP_V3HEADER_SIZE 56    ///< BITMAPV3INFOHEADER (adds alpha mask).
#define GIMG_BMP_V4HEADER_SIZE 108   ///< BITMAPV4HEADER.
#define GIMG_BMP_V5HEADER_SIZE 124   ///< BITMAPV5HEADER.

/** @name bV4CSType / bV5CSType values, four-character codes where they are.
 * @{ */
#define GIMG_BMP_LCS_CALIBRATED_RGB UINT32_C(0x00000000) ///< Endpoints + gamma.
#define GIMG_BMP_LCS_sRGB UINT32_C(0x73524742) ///< 'sRGB'.
#define GIMG_BMP_LCS_WINDOWS_COLOR_SPACE UINT32_C(0x57696E20) ///< 'Win '.
#define GIMG_BMP_PROFILE_LINKED UINT32_C(0x4C494E4B) ///< 'LINK': a file path.
#define GIMG_BMP_PROFILE_EMBEDDED UINT32_C(0x4D424544) ///< 'MBED': a profile.
/** @} */


/** Longest PROFILE_LINKED path this codec will read.
 *
 * bV5ProfileSize is 32 bits, and for a linked profile it measures a file name
 * rather than a profile.  Four kibibytes is past any path a filesystem will
 * accept, so a header naming more than this is describing something that is
 * not a path and the field is ignored. */
#define GIMG_BMP_LINKED_PATH_MAX 4096u

/** Where a V4 header's color fields begin: after the four channel masks,
 * which a V4 header carries whether or not the compression uses them. */
#define GIMG_BMP_V4_TAIL_AT 56

/** @name Color fields, as offsets into that tail rather than into the header,
 * because the writer builds the tail on its own and drops it into place.
 * @{ */
#define GIMG_BMP_V4_CS_TYPE_AT 0    ///< bV4CSType (absolute 56).
#define GIMG_BMP_V4_ENDPOINTS_AT 4  ///< bV4Endpoints, 9 x FXPT2DOT30 (60).
#define GIMG_BMP_V4_GAMMA_AT 40     ///< bV4GammaRed, then green, blue (96).
#define GIMG_BMP_V5_INTENT_AT 52    ///< bV5Intent (108).
#define GIMG_BMP_V5_PROFILE_AT 56   ///< bV5ProfileData, then size (112).
/** @} */

/** Smallest and largest BITMAPCOREHEADER2 (OS/2 2.x); any multiple of 4
 * between them is legal, and a field the header stops short of reads as
 * zero. */
#define GIMG_BMP_OS2V2_MIN_SIZE 16
#define GIMG_BMP_OS2V2_MAX_SIZE 64

/** biCompression values, as a Windows BITMAPINFOHEADER spells them. */
#define GIMG_BMP_BI_RGB 0u
#define GIMG_BMP_BI_RLE8 1u
#define GIMG_BMP_BI_RLE4 2u
#define GIMG_BMP_BI_BITFIELDS 3u
#define GIMG_BMP_BI_JPEG 4u
#define GIMG_BMP_BI_PNG 5u
#define GIMG_BMP_BI_ALPHABITFIELDS 6u

/** ulCompression values, as an OS/2 BITMAPCOREHEADER2 spells them.  0, 1 and
 * 2 agree with Windows; 3 and 4 do not, which is why the two spellings are
 * normalized into gimg_bmp_compression_t rather than compared raw. */
#define GIMG_BMP_OS2_HUFFMAN1D 3u
#define GIMG_BMP_OS2_RLE24 4u

/**
 * @brief How the pixel data is stored, independent of which header spelled it.
 *
 * A 64-byte OS/2 header and a 40-byte Windows one share their first 40 bytes
 * but not their compression numbering: 3 is BI_BITFIELDS to Windows and
 * Huffman 1D to OS/2, and 4 is BI_JPEG to Windows and RLE24 to OS/2.  Keeping
 * the raw number around invites a comparison against the wrong vocabulary, so
 * the header reader resolves it once and everything downstream reads this.
 */
typedef enum {
  GIMG_BMP_COMP_RGB = 0,   ///< Uncompressed, channel layout implied by depth.
  GIMG_BMP_COMP_RLE8,      ///< 8-bit run-length encoding.
  GIMG_BMP_COMP_RLE4,      ///< 4-bit run-length encoding.
  GIMG_BMP_COMP_RLE24,     ///< 24-bit run-length encoding (OS/2 2.x).
  GIMG_BMP_COMP_HUFFMAN1D, ///< CCITT Group 3 one-dimensional (OS/2 2.x).
  GIMG_BMP_COMP_BITFIELDS, ///< Uncompressed, channel layout given by masks.
  GIMG_BMP_COMP_JPEG,      ///< The pixel data is a whole JPEG stream.
  GIMG_BMP_COMP_PNG        ///< The pixel data is a whole PNG stream.
} gimg_bmp_compression_t;

/**
 * @brief A single channel's extraction rule for a BI_BITFIELDS image.
 *
 * `shift` is the number of low zero bits in the mask and `max` is the mask's
 * value once shifted down, so a sample is
 * `((pixel & mask) >> shift) * 255 / max`.
 */
typedef struct {
  uint32_t mask;
  unsigned int shift;
  unsigned int max;
} gimg_bmp_channel_mask_t;

/**
 * @brief Parsed DIB header, normalized across the header versions.
 */
typedef struct {
  uint32_t header_size;   ///< Size of the DIB header as read from the file.
  uint32_t width;         ///< Image width in pixels (always positive).
  uint32_t height;        ///< Image height in pixels (always positive).
  bool top_down;          ///< True when the file stored rows top to bottom.
  uint16_t bit_count;     ///< Bits per pixel: 1, 2, 4, 8, 16, 24, 32, or 64.
  bool os2_v2;            ///< True for a BITMAPCOREHEADER2 (OS/2 2.x).
  gimg_bmp_compression_t compression; ///< How the pixel data is stored.
  uint32_t size_image;    ///< biSizeImage as read; untrusted, 0 means unset.
  uint32_t x_ppm;         ///< biXPelsPerMeter; 0 means the file did not say.
  uint32_t y_ppm;         ///< biYPelsPerMeter; 0 means the file did not say.
  uint32_t palette_count; ///< Palette entries actually present in the file.
  bool has_masks;         ///< True when the channel masks below are in use.
  gimg_bmp_channel_mask_t red;
  gimg_bmp_channel_mask_t green;
  gimg_bmp_channel_mask_t blue;
  gimg_bmp_channel_mask_t alpha; ///< `mask` is 0 when the format has no alpha.

  // BITMAPV4HEADER and BITMAPV5HEADER color fields; zero before V4.
  uint32_t cs_type;        ///< bV4CSType: LCS_*, PROFILE_LINKED/EMBEDDED.
  int32_t endpoints[9];    ///< bV4Endpoints, as FXPT2DOT30.
  uint32_t gamma[3];       ///< bV4Gamma{Red,Green,Blue}, as 16.16 fixed point.
  uint32_t intent;         ///< bV5Intent (LCS_GM_*); 0 before V5.
  uint32_t profile_offset; ///< bV5ProfileData, from the DIB header's start.
  uint32_t profile_size;   ///< bV5ProfileSize.
} gimg_bmp_header_t;

/** @brief True for the compressions whose pixel data is a whole other image. */
static inline bool gimg_bmp_is_embedded(gimg_bmp_compression_t c) {
  return c == GIMG_BMP_COMP_JPEG || c == GIMG_BMP_COMP_PNG;
}

/** @brief True for the compressions whose pixel data is a run-length stream. */
static inline bool gimg_bmp_is_rle(gimg_bmp_compression_t c) {
  return c == GIMG_BMP_COMP_RLE8 || c == GIMG_BMP_COMP_RLE4 ||
      c == GIMG_BMP_COMP_RLE24;
}

/**
 * @brief A palette entry, already expanded to 8 bits per channel.
 */
typedef struct {
  uint8_t r;
  uint8_t g;
  uint8_t b;
} gimg_bmp_palette_entry_t;

/**
 * @brief Document state produced by load and consumed by decode.
 *
 * Load performs no pixel work: it validates the headers, copies the palette,
 * and holds the raw pixel bytes.  Decode turns those into a raster.  Keeping
 * the split means a caller that only wants dimensions or metadata never pays
 * for the conversion.
 */
typedef struct {
  const GIMG_Allocator * allocator;
  gimg_bmp_header_t header;
  gimg_bmp_palette_entry_t * palette; ///< NULL when bit_count > 8.
  uint32_t palette_count;
  unsigned char * pixels; ///< Raw pixel bytes as stored in the file.
  size_t pixels_size;
  /** The color the header described, if it described one this model can
   * hold.  Built at load and attached to the raster at decode, so that a
   * caller who only wants the dimensions never pays for it. */
  GIMG_Color_Info color;
  void * icc;      ///< An embedded ICC profile, owned here; NULL when none.
  size_t icc_size;
  /** The path a PROFILE_LINKED file named, owned here, NUL-terminated; NULL
   * when the file carried no such path or a resolver supplied the profile. */
  char * icc_linked_path;
  /** For BI_JPEG and BI_PNG, the document the embedded stream loaded into.
   * Decode hands the work to its first item rather than doing any of its own:
   * the "pixel data" of such a file is a whole JPEG or PNG, and this library
   * has a codec for each. */
  GIMG_Doc * embedded;
  /** For an OS/2 'BA' container, one loaded document per entry, each holding
   * an ordinary bitmap.  The header, palette and pixel fields above are unused
   * when this is set: a container holds no pixels of its own, and decode
   * forwards to the entry the item index names.  The entries are alternative
   * renderings of one picture for different devices, so they are exposed as
   * items and the choice of which to use is left to the caller. */
  GIMG_Doc ** array_entries;
  size_t array_count;
  /** GIMG_Load_Options.bmp_rgb32_alpha as the load was given it.  Kept here
   * rather than read again at decode because a save re-decodes its source
   * item with no options of its own, and must not reinterpret the pixels
   * differently from the load that produced them. */
  uint8_t rgb32_alpha;
} gimg_bmp_doc_state_t;

/**
 * @brief Fill in what a V4 or V5 header's color fields say, as far as
 *   GIMG_Color_Info can hold it.
 *
 * Anything the model cannot state is left unknown rather than approximated.
 * The ICC profile, which lives outside the header, is not touched here.
 *
 * @param header Parsed header.
 * @param out_info Receives the color; set to defaults when the header says
 *   nothing this can hold.
 */
void gimg_bmp_color_from_header(
    const gimg_bmp_header_t * header, GIMG_Color_Info * out_info);

/**
 * @brief Write a little-endian 32-bit field.
 *
 * Shared because both the header writer and the color writer build headers a
 * field at a time.
 */
static inline void gimg_bmp_write_u32(unsigned char * p, uint32_t value) {
  p[0] = (unsigned char)(value & 0xFFu);
  p[1] = (unsigned char)((value >> 8) & 0xFFu);
  p[2] = (unsigned char)((value >> 16) & 0xFFu);
  p[3] = (unsigned char)((value >> 24) & 0xFFu);
}

/**
 * @brief Say what a V4 or V5 header would state about a raster's color, and
 *   build the part of the header that states it.
 *
 * The inverse of gimg_bmp_color_from_header, and bounded the same way: only
 * what GIMG_Color_Info holds is written, and a color this model cannot state
 * produces no header at all rather than the nearest thing it can say.
 *
 * @param info Color to state; NULL or an empty one says nothing.
 * @param tail Receives bytes @ref GIMG_BMP_V4_TAIL_AT onwards of the header -
 *   @ref GIMG_BMP_V5HEADER_SIZE minus that many bytes - zeroed first.  When
 *   the return value is @ref GIMG_BMP_V5HEADER_SIZE and an ICC profile is to
 *   be embedded, bV5ProfileData and bV5ProfileSize at
 *   @ref GIMG_BMP_V5_PROFILE_AT are left at zero for the caller to fill:
 *   where the profile lands depends on how much pixel data precedes it.
 * @return The DIB header size that carries what was written -
 *   @ref GIMG_BMP_V4HEADER_SIZE or @ref GIMG_BMP_V5HEADER_SIZE - or 0 when
 *   there was nothing to say and the smallest header will do.
 */
uint32_t gimg_bmp_color_to_header(
    const GIMG_Color_Info * info, unsigned char * tail);

/**
 * @brief Read an embedded ICC profile out of a PROFILE_EMBEDDED V5 file.
 *
 * The stream is left where it was found.  A profile that runs off the end of
 * the file, or cannot be read, yields no profile rather than an error: the
 * image decodes perfectly well untagged, and refusing a picture over its
 * color annotation would be the wrong trade.
 *
 * @param stream Stream positioned anywhere; restored before returning.
 * @param header Parsed header.
 * @param limits Caller's limits, or NULL.
 * @param alloc Allocator for the profile bytes.
 * @param out_profile Receives the profile, owned by the caller; NULL if none.
 * @param out_size Receives its length; 0 if none.
 * @return GIMG_OK, GIMG_ERR_LIMIT if the profile exceeds max_memory, or
 *   GIMG_ERR_OOM.
 */
GIMG_Result gimg_bmp_read_profile(GIMG_Stream * stream,
    const gimg_bmp_header_t * header, const GIMG_Limits * limits,
    const GIMG_Allocator * alloc, void ** out_profile, size_t * out_size);

/**
 * @brief Read the file path a PROFILE_LINKED V5 header names.
 *
 * The path is read and never opened.  The bytes are taken as stored and
 * NUL-terminated here, because the format states no encoding for them and the
 * only published example holds a byte that is not ASCII.
 *
 * @param stream Stream positioned anywhere; restored before returning.
 * @param header Parsed header.
 * @param alloc Allocator for the path bytes.
 * @param out_path Receives the path, owned by the caller; NULL if none.
 * @return GIMG_OK - a path that cannot be read yields none rather than
 *   refusing the image, the same trade an unreadable embedded profile gets -
 *   or GIMG_ERR_OOM.
 */
GIMG_Result gimg_bmp_read_linked_path(GIMG_Stream * stream,
    const gimg_bmp_header_t * header, const GIMG_Allocator * alloc,
    char ** out_path);

/**
 * @brief Read and verify the "BM" signature, leaving the stream after it.
 * @return GIMG_OK, GIMG_ERR_FORMAT if the magic does not match, or an I/O
 *   error.
 */
/**
 * @brief Expand a CCITT Group 3 one-dimensional stream into packed 1-bit rows.
 *
 * Writes exactly what an uncompressed 1-bit image of the same size would have
 * held, so that everything downstream - palette lookup, row order, limits -
 * needs no knowledge that the file was compressed at all.
 *
 * @param data Encoded bytes.
 * @param size Their length.
 * @param width Image width in pixels.
 * @param height Image height in rows.
 * @param stride Padded row size of the destination, from gimg_bmp_row_stride.
 * @param out Receives `stride * height` bytes; zeroed first.
 * @return GIMG_OK, or GIMG_ERR_CORRUPT if the stream ends early or holds bits
 *   no code matches.
 */
GIMG_Result gimg_bmp_huffman_expand(const unsigned char * data, size_t size,
    uint32_t width, uint32_t height, size_t stride, unsigned char * out);

/**
 * @brief Encode packed 1-bit rows as a CCITT Group 3 one-dimensional stream.
 *
 * The inverse of gimg_bmp_huffman_expand, over the same tables.  A set bit is
 * black, which is the polarity q/pal1huffmsb.bmp and g/pal1.bmp settle between
 * them.
 *
 * @param rows Packed 1-bit rows, in the order the file will store them.
 * @param width Image width in pixels.
 * @param height Image height in rows.
 * @param stride Padded row size of the source.
 * @param out Receives the encoded bytes; NULL to measure only.
 * @param capacity Bytes available at @a out; ignored when measuring.
 * @param out_size Receives the encoded length, whether measuring or writing.
 * @return GIMG_OK, GIMG_ERR_LIMIT if the buffer was short, or
 *   GIMG_ERR_INTERNAL if a run had no code, which cannot happen for runs
 *   inside a row.
 */
GIMG_Result gimg_bmp_huffman_encode(const unsigned char * rows, uint32_t width,
    uint32_t height, size_t stride, unsigned char * out, size_t capacity,
    size_t * out_size);

GIMG_Result gimg_bmp_verify_signature(GIMG_Stream * stream);

/** @brief Load callback: stream -> document with gimg_bmp_doc_state_t. */
GIMG_Result gimg_bmp_load(GIMG_Codec * codec, GIMG_Stream * stream,
    const GIMG_Load_Options * options, GIMG_Diagnostics * diagnostics,
    GIMG_Doc ** out_doc);

/** @brief Decode callback: document state -> RGBA8 raster. */
GIMG_Result gimg_bmp_decode(GIMG_Codec * codec, const GIMG_Item * item,
    const GIMG_Decode_Options * options, GIMG_Raster ** out_raster);

/** @brief Save callback: document -> uncompressed BMP. */
GIMG_Result gimg_bmp_save(GIMG_Codec * codec, const GIMG_Doc * doc,
    GIMG_Stream * stream, const char * format_name,
    const GIMG_Save_Options * options, GIMG_Save_Report * report);

/** @brief Release the document state attached by load. */
void gimg_bmp_free_doc_state(GIMG_Codec * codec, void * codec_private);

/**
 * @brief Bytes per row of pixel data, including the 4-byte row padding.
 *
 * @param width Image width in pixels.
 * @param bit_count Bits per pixel.
 * @param out_stride Receives the padded row size.
 * @return GIMG_OK, or GIMG_ERR_LIMIT if the computation overflows.
 */
GIMG_Result gimg_bmp_row_stride(
    uint32_t width, uint16_t bit_count, size_t * out_stride);

#endif // GHOTI_IO_GIMG_SRC_CODEC_BMP_BMP_INTERNAL_H

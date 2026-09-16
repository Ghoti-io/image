/**
 * @file
 *
 * Internal BMP codec structures: DIB header fields, channel masks, and the
 * document state carried from load to decode.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#ifndef GHOTI_IO_GIMG_SRC_CODEC_BMP_BMP_INTERNAL_H
#define GHOTI_IO_GIMG_SRC_CODEC_BMP_BMP_INTERNAL_H

#include <ghoti.io/image/macros.h>

#include <ghoti.io/image/allocator.h>
#include <ghoti.io/image/codec.h>
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

/** Size of the BITMAPFILEHEADER on disk. */
#define GIMG_BMP_FILE_HEADER_SIZE 14

/** DIB header sizes we recognize. */
#define GIMG_BMP_COREHEADER_SIZE 12  ///< BITMAPCOREHEADER (OS/2 1.x).
#define GIMG_BMP_INFOHEADER_SIZE 40  ///< BITMAPINFOHEADER (Windows 3.x).
#define GIMG_BMP_V2HEADER_SIZE 52    ///< BITMAPV2INFOHEADER (RGB masks).
#define GIMG_BMP_V3HEADER_SIZE 56    ///< BITMAPV3INFOHEADER (adds alpha mask).
#define GIMG_BMP_V4HEADER_SIZE 108   ///< BITMAPV4HEADER.
#define GIMG_BMP_V5HEADER_SIZE 124   ///< BITMAPV5HEADER.

/** biCompression values. */
#define GIMG_BMP_BI_RGB 0u
#define GIMG_BMP_BI_RLE8 1u
#define GIMG_BMP_BI_RLE4 2u
#define GIMG_BMP_BI_BITFIELDS 3u
#define GIMG_BMP_BI_ALPHABITFIELDS 6u

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
  uint16_t bit_count;     ///< Bits per pixel: 1, 2, 4, 8, 16, 24, or 32.
  uint32_t compression;   ///< One of the GIMG_BMP_BI_* values.
  uint32_t palette_count; ///< Palette entries actually present in the file.
  bool has_masks;         ///< True when the channel masks below are in use.
  gimg_bmp_channel_mask_t red;
  gimg_bmp_channel_mask_t green;
  gimg_bmp_channel_mask_t blue;
  gimg_bmp_channel_mask_t alpha; ///< `mask` is 0 when the format has no alpha.
} gimg_bmp_header_t;

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
} gimg_bmp_doc_state_t;

/**
 * @brief Read and verify the "BM" signature, leaving the stream after it.
 * @return GIMG_OK, GIMG_ERR_FORMAT if the magic does not match, or an I/O
 *   error.
 */
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

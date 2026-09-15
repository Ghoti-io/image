/**
 * @file
 *
 * BMP save: raster -> uncompressed BITMAPINFOHEADER bitmap.
 *
 * Copyright 2026 by Corey Pennycuff
 *
 * --- Internal algorithms and design ---
 *
 * Depth selection: a raster whose alpha is not uniformly opaque is written as
 * 32-bit BI_BITFIELDS with an explicit alpha mask, since 32-bit BI_RGB leaves
 * the fourth byte undefined and readers disagree about it.  Everything else
 * is written as 24-bit BI_RGB, which is the most widely readable BMP there
 * is.  The caller can force 32-bit by leaving a non-opaque alpha in place.
 *
 * Row order: rows are written bottom-up with a positive height, the layout
 * every BMP reader handles.  Top-down BMPs exist but are less portable and
 * are illegal in combination with compression.
 *
 * Source formats: the raster is read through gimg_raster_* accessors for RGBA8
 * and GRAY8, the two 8-bit formats the library decodes to.  Anything else is
 * reported as unsupported rather than reinterpreted.
 */

#include <ghoti.io/image/codec.h>
#include <ghoti.io/image/core.h>
#include <ghoti.io/image/doc.h>
#include <ghoti.io/image/raster.h>
#include <string.h>

#include "../../container/doc_internal.h"
#include "../../core/alloc_internal.h"
#include "../../core/safe_math_internal.h"
#include "../codec_internal.h"
#include "bmp_internal.h"

/** Append a little-endian 16-bit value to a byte cursor. */
static void bmp_write_u16(unsigned char * p, uint16_t value) {
  p[0] = (unsigned char)(value & 0xFFu);
  p[1] = (unsigned char)((value >> 8) & 0xFFu);
}

/** Append a little-endian 32-bit value to a byte cursor. */
static void bmp_write_u32(unsigned char * p, uint32_t value) {
  p[0] = (unsigned char)(value & 0xFFu);
  p[1] = (unsigned char)((value >> 8) & 0xFFu);
  p[2] = (unsigned char)((value >> 16) & 0xFFu);
  p[3] = (unsigned char)((value >> 24) & 0xFFu);
}

/** Write a whole buffer, treating a short write as an I/O error. */
static GIMG_Result bmp_write_all(
    GIMG_Stream * stream, const void * buffer, size_t size) {
  size_t written = 0;
  GIMG_Result r = gimg_stream_write(stream, buffer, size, &written);
  if (r != GIMG_OK) {
    return r;
  }
  return written == size ? GIMG_OK : GIMG_ERR_IO;
}

/** True when the format is one this encoder can read samples from. */
static bool bmp_format_supported(const GIMG_Pixel_Format * f) {
  if (!f || f->layout != GIMG_LAYOUT_INTERLEAVED ||
      f->channel_type != GIMG_CHANNEL_UNORM) {
    return false;
  }
  if (f->channel_model == GIMG_CHANNEL_RGBA && f->channel_count == 4 &&
      f->bits_per_channel[0] == 8) {
    return true;
  }
  if (f->channel_model == GIMG_CHANNEL_GRAY && f->channel_count == 1 &&
      f->bits_per_channel[0] == 8) {
    return true;
  }
  return false;
}

/** Read one pixel as RGBA8 from a supported raster format. */
static void bmp_sample(const GIMG_Pixel_Format * f, const uint8_t * row,
    uint32_t x, uint8_t out[4]) {
  if (f->channel_model == GIMG_CHANNEL_GRAY) {
    uint8_t v = row[x];
    out[0] = v;
    out[1] = v;
    out[2] = v;
    out[3] = 255u;
    return;
  }
  const uint8_t * px = row + ((size_t)x * 4u);
  out[0] = px[0];
  out[1] = px[1];
  out[2] = px[2];
  out[3] = px[3];
}

/** True when every pixel's alpha is 255. */
static bool bmp_raster_is_opaque(const GIMG_Raster * raster) {
  const GIMG_Pixel_Format * f = gimg_raster_format(raster);
  if (f->channel_model != GIMG_CHANNEL_RGBA) {
    return true;
  }
  uint32_t width = gimg_raster_width(raster);
  uint32_t height = gimg_raster_height(raster);
  size_t stride = gimg_raster_stride_bytes(raster);
  const uint8_t * pixels = (const uint8_t *)gimg_raster_pixels_const(raster);

  for (uint32_t y = 0; y < height; y++) {
    const uint8_t * row = pixels + ((size_t)y * stride);
    for (uint32_t x = 0; x < width; x++) {
      if (row[((size_t)x * 4u) + 3u] != 255u) {
        return false;
      }
    }
  }
  return true;
}

GIMG_Result gimg_bmp_save(GIMG_Codec * codec, const GIMG_Doc * doc,
    GIMG_Stream * stream, const char * format_name,
    const GIMG_Save_Options * options, GIMG_Save_Report * report) {
  (void)format_name;
  (void)options;
  if (!codec || !doc || !stream || !report) {
    return GIMG_ERR_INTERNAL;
  }
  report->bytes_written = 0;

  if (gimg_doc_item_count(doc) == 0) {
    return GIMG_ERR_FORMAT;
  }
  GIMG_Item * item = gimg_doc_item((GIMG_Doc *)doc, 0);
  if (!item) {
    return GIMG_ERR_INTERNAL;
  }

  // Prefer a raster already attached to the item; otherwise decode one and
  // take ownership of it for the duration of the save.
  GIMG_Raster * raster = gimg_item_raster(item);
  bool raster_owned = false;
  if (!raster) {
    GIMG_Result dr = gimg_item_decode(item, NULL, &raster);
    if (dr != GIMG_OK || !raster) {
      return dr == GIMG_ERR_UNSUPPORTED || dr == GIMG_OK ? GIMG_ERR_FORMAT : dr;
    }
    raster_owned = true;
  }

  GIMG_Result result = GIMG_OK;
  const GIMG_Allocator * alloc = gimg_alloc_or_default(codec->allocator);
  unsigned char * row_buffer = NULL;

  const GIMG_Pixel_Format * format = gimg_raster_format(raster);
  if (!bmp_format_supported(format)) {
    result = GIMG_ERR_UNSUPPORTED;
    goto done;
  }

  {
    uint32_t width = gimg_raster_width(raster);
    uint32_t height = gimg_raster_height(raster);
    if (!width || !height) {
      result = GIMG_ERR_FORMAT;
      goto done;
    }

    bool opaque = bmp_raster_is_opaque(raster);
    uint16_t bit_count = opaque ? 24u : 32u;
    // An alpha channel needs explicit masks; 32-bit BI_RGB does not define
    // the fourth byte and readers disagree on whether to honor it.
    uint32_t compression = opaque ? GIMG_BMP_BI_RGB : GIMG_BMP_BI_BITFIELDS;
    uint32_t dib_size =
        opaque ? GIMG_BMP_INFOHEADER_SIZE : GIMG_BMP_V3HEADER_SIZE;

    size_t stride;
    result = gimg_bmp_row_stride(width, bit_count, &stride);
    if (result != GIMG_OK) {
      goto done;
    }

    size_t pixel_bytes;
    if (!gcu_safe_mul_size(stride, (size_t)height, &pixel_bytes)) {
      result = GIMG_ERR_LIMIT;
      goto done;
    }

    size_t data_offset = (size_t)GIMG_BMP_FILE_HEADER_SIZE + (size_t)dib_size;
    size_t file_size;
    if (!gcu_safe_add_size(data_offset, pixel_bytes, &file_size) ||
        file_size > UINT32_MAX) {
      result = GIMG_ERR_LIMIT;
      goto done;
    }

    // File header.
    unsigned char file_header[GIMG_BMP_FILE_HEADER_SIZE];
    memset(file_header, 0, sizeof(file_header));
    file_header[0] = gimg_bmp_signature[0];
    file_header[1] = gimg_bmp_signature[1];
    bmp_write_u32(file_header + 2, (uint32_t)file_size);
    bmp_write_u32(file_header + 10, (uint32_t)data_offset);
    result = bmp_write_all(stream, file_header, sizeof(file_header));
    if (result != GIMG_OK) {
      goto done;
    }

    // DIB header.
    unsigned char dib[GIMG_BMP_V3HEADER_SIZE];
    memset(dib, 0, sizeof(dib));
    bmp_write_u32(dib + 0, dib_size);
    bmp_write_u32(dib + 4, width);
    bmp_write_u32(dib + 8, height); // Positive: rows are stored bottom-up.
    bmp_write_u16(dib + 12, 1u);    // Planes.
    bmp_write_u16(dib + 14, bit_count);
    bmp_write_u32(dib + 16, compression);
    bmp_write_u32(dib + 20, (uint32_t)pixel_bytes);
    bmp_write_u32(dib + 24, 2835u); // 72 DPI in pixels per meter.
    bmp_write_u32(dib + 28, 2835u);
    if (!opaque) {
      bmp_write_u32(dib + 40, 0x00FF0000u); // Red.
      bmp_write_u32(dib + 44, 0x0000FF00u); // Green.
      bmp_write_u32(dib + 48, 0x000000FFu); // Blue.
      bmp_write_u32(dib + 52, 0xFF000000u); // Alpha.
    }
    result = bmp_write_all(stream, dib, dib_size);
    if (result != GIMG_OK) {
      goto done;
    }

    // Pixel rows, bottom-up, zero-padded to a 4-byte boundary.
    row_buffer = (unsigned char *)gimg_calloc(alloc, 1, stride);
    if (!row_buffer) {
      result = GIMG_ERR_OOM;
      goto done;
    }

    size_t src_stride = gimg_raster_stride_bytes(raster);
    const uint8_t * pixels =
        (const uint8_t *)gimg_raster_pixels_const(raster);

    for (uint32_t y = 0; y < height; y++) {
      const uint8_t * src = pixels + ((size_t)(height - 1u - y) * src_stride);
      memset(row_buffer, 0, stride);
      for (uint32_t x = 0; x < width; x++) {
        uint8_t rgba[4];
        bmp_sample(format, src, x, rgba);
        unsigned char * out =
            row_buffer + ((size_t)x * (size_t)(bit_count / 8u));
        out[0] = rgba[2]; // Blue.
        out[1] = rgba[1]; // Green.
        out[2] = rgba[0]; // Red.
        if (bit_count == 32) {
          out[3] = rgba[3];
        }
      }
      result = bmp_write_all(stream, row_buffer, stride);
      if (result != GIMG_OK) {
        goto done;
      }
    }

    report->bytes_written = file_size;
  }

done:
  gimg_free(alloc, row_buffer);
  if (raster_owned) {
    gimg_raster_destroy(raster);
  }
  return result;
}

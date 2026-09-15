/**
 * @file
 *
 * PNG save: encode raster to PNG (signature, IHDR, ancillary, IDAT, IEND).
 * Uses DEFLATE via compress library; zlib-wraps for IDAT (2-byte header +
 * raw DEFLATE + 4-byte Adler-32 per RFC 1950).
 *
 * Copyright 2026 by Corey Pennycuff
 *
 * --- Internal algorithms and design ---
 *
 * Chunk order: We emit chunks in PNG spec order (see format-references.md):
 * signature, IHDR, ancillary (per GIMG_Meta_Policy), PLTE/tRNS if palette,
 * IDAT (one or multiple), IEND. Ancillary is written in the order stored
 * during load (read order) when policy is
 * PRESERVE_ALL/STRIP_GPS/NORMALIZE_EXIF.
 *
 * Filter: All rows use filter type 0 (None). A heuristic (e.g. Sub/Up/Average/
 * Paeth) could be added later to improve compression; raw bytes are passed to
 * DEFLATE as-is.
 *
 * DEFLATE: We use the compress library's "deflate" method with strategy
 * "filtered" (zlib-friendly). The raw image buffer (filter byte + row data
 * per row, or Adam7 pass order when interlaced) is compressed in one shot;
 * then we zlib-wrap (RFC 1950): 2-byte header (0x78 0x9C), raw DEFLATE bytes,
 * 4-byte Adler-32 of the uncompressed data (big-endian).
 *
 * IDAT splitting: The zlib payload is written as one or more IDAT chunks with
 * a maximum of 32 KiB per chunk. Some decoders expect smaller IDATs; splitting
 * avoids compatibility issues while keeping chunk count low.
 *
 * Palette round-trip: When the doc was loaded from a palette PNG (state has
 * PLTE/tRNS), we encode as palette if the raster is RGBA8 and every pixel
 * matches a PLTE entry (and tRNS alpha when present). Matching is exact; no
 * quantization. If any pixel has no match we fall back to unsupported (caller
 * would need to requantize or use RGB).
 *
 * Interlace (Adam7): When options->interlaced is set, we fill raw rows in
 * Adam7 pass order (seven passes per W3C §2.6), each row prefixed with filter
 * byte 0, then DEFLATE the entire interlaced buffer.
 */

#include <ctype.h>
#include <ghoti.io/image/codec.h>
#include <ghoti.io/image/color.h>
#include <ghoti.io/image/doc.h>
#include <ghoti.io/image/meta.h>
#include <ghoti.io/image/raster.h>
#include <ghoti.io/image/stream.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <ghoti.io/compress/compress.h>
#include <ghoti.io/compress/options.h>

#include "../../container/doc_internal.h"
#include "../../core/alloc_internal.h"
#include "../../core/safe_math_internal.h"
#include "../../meta/exif_internal.h"
#include "../../raster/raster_internal.h"
#include "../codec_internal.h"
#include "png_internal.h"

/** Adler-32 modulus (RFC 1950). */
#define GIMG_PNG_ADLER_MOD 65521u

/** Compute Adler-32 over data (RFC 1950). */
static uint32_t gimg_png_adler32(const unsigned char * data, size_t len) {
  uint32_t s1 = 1u;
  uint32_t s2 = 0u;
  for (size_t i = 0; i < len; i++) {
    s1 = (s1 + (uint32_t)data[i]) % GIMG_PNG_ADLER_MOD;
    s2 = (s2 + s1) % GIMG_PNG_ADLER_MOD;
  }
  return (s2 << 16) | s1;
}

/**
 * Map raster format to PNG color_type and bit_depth.
 * Returns 1 on success, 0 on unsupported. Does not handle palette (caller uses
 * doc state for that). When @a state is non-NULL and state->ihdr.color_type is
 * 2 (RGB) or 4 (grayscale+alpha), and the raster is RGBA with matching bit
 * depth, that color_type is used so round-trip preserves format.
 */
static bool gimg_png_raster_to_ihdr(
    const GIMG_Raster * raster, const gimg_png_doc_state_t * state,
    uint8_t * color_type, uint8_t * bit_depth) {
  const GIMG_Pixel_Format * fmt = gimg_raster_format(raster);
  if (!fmt || fmt->layout != GIMG_LAYOUT_INTERLEAVED) {
    return false;
  }
  if (fmt->channel_model == GIMG_CHANNEL_GRAY && fmt->channel_count >= 1) {
    if (fmt->bits_per_channel[0] == 8) {
      *color_type = 0;
      *bit_depth = 8;
      return true;
    }
    if (fmt->bits_per_channel[0] == 16) {
      *color_type = 0;
      *bit_depth = 16;
      return true;
    }
    return false;
  }
  if (fmt->channel_model == GIMG_CHANNEL_RGBA && fmt->channel_count == 4) {
    uint8_t bd = 0;
    if (fmt->bits_per_channel[0] == 8) {
      bd = 8;
    }
    else if (fmt->bits_per_channel[0] == 16) {
      bd = 16;
    }
    else {
      return false;
    }
    if (state && (state->ihdr.color_type == 2 || state->ihdr.color_type == 4) &&
        state->ihdr.bit_depth == bd) {
      *color_type = state->ihdr.color_type;
      *bit_depth = bd;
      return true;
    }
    *color_type = 6;
    *bit_depth = bd;
    return true;
  }
  return false;
}

/** Build IHDR payload (13 bytes). @a interlace_method 0 or 1 (Adam7). */
static void gimg_png_build_ihdr(unsigned char * out, uint32_t width,
    uint32_t height, uint8_t bit_depth, uint8_t color_type,
    uint8_t interlace_method) {
  out[0] = (unsigned char)(width >> 24);
  out[1] = (unsigned char)(width >> 16);
  out[2] = (unsigned char)(width >> 8);
  out[3] = (unsigned char)(width & 0xFF);
  out[4] = (unsigned char)(height >> 24);
  out[5] = (unsigned char)(height >> 16);
  out[6] = (unsigned char)(height >> 8);
  out[7] = (unsigned char)(height & 0xFF);
  out[8] = bit_depth;
  out[9] = color_type;
  out[10] = 0; // compression_method
  out[11] = 0; // filter_method
  out[12] = interlace_method;
}

/** Build acTL payload (8 bytes): num_frames, num_plays (big-endian). */
static void gimg_png_build_actl(
    unsigned char * out, uint32_t num_frames, uint32_t num_plays) {
  out[0] = (unsigned char)(num_frames >> 24);
  out[1] = (unsigned char)(num_frames >> 16);
  out[2] = (unsigned char)(num_frames >> 8);
  out[3] = (unsigned char)(num_frames & 0xFFu);
  out[4] = (unsigned char)(num_plays >> 24);
  out[5] = (unsigned char)(num_plays >> 16);
  out[6] = (unsigned char)(num_plays >> 8);
  out[7] = (unsigned char)(num_plays & 0xFFu);
}

/** Build fcTL payload (26 bytes) from frame dimensions and item timing. */
static void gimg_png_build_fctl(unsigned char * out, uint32_t sequence_number,
    uint32_t width, uint32_t height, uint32_t x_offset, uint32_t y_offset,
    uint16_t delay_num, uint16_t delay_den, uint8_t dispose_op,
    uint8_t blend_op) {
  out[0] = (unsigned char)(sequence_number >> 24);
  out[1] = (unsigned char)(sequence_number >> 16);
  out[2] = (unsigned char)(sequence_number >> 8);
  out[3] = (unsigned char)(sequence_number & 0xFFu);
  out[4] = (unsigned char)(width >> 24);
  out[5] = (unsigned char)(width >> 16);
  out[6] = (unsigned char)(width >> 8);
  out[7] = (unsigned char)(width & 0xFFu);
  out[8] = (unsigned char)(height >> 24);
  out[9] = (unsigned char)(height >> 16);
  out[10] = (unsigned char)(height >> 8);
  out[11] = (unsigned char)(height & 0xFFu);
  out[12] = (unsigned char)(x_offset >> 24);
  out[13] = (unsigned char)(x_offset >> 16);
  out[14] = (unsigned char)(x_offset >> 8);
  out[15] = (unsigned char)(x_offset & 0xFFu);
  out[16] = (unsigned char)(y_offset >> 24);
  out[17] = (unsigned char)(y_offset >> 16);
  out[18] = (unsigned char)(y_offset >> 8);
  out[19] = (unsigned char)(y_offset & 0xFFu);
  out[20] = (unsigned char)(delay_num >> 8);
  out[21] = (unsigned char)(delay_num & 0xFFu);
  out[22] = (unsigned char)(delay_den >> 8);
  out[23] = (unsigned char)(delay_den & 0xFFu);
  out[24] = dispose_op;
  out[25] = blend_op;
}

/** Return true if chunk type is known semantic metadata (color, Exif, text). */
static bool gimg_png_chunk_is_known_semantic(gimg_png_chunk_type_t t) {
  return t == GIMG_PNG_iCCP || t == GIMG_PNG_sRGB || t == GIMG_PNG_gAMA ||
      t == GIMG_PNG_cHRM || t == GIMG_PNG_eXIf || t == GIMG_PNG_tEXt ||
      t == GIMG_PNG_zTXt || t == GIMG_PNG_iTXt;
}

/**
 * Return true if the text chunk payload has keyword "Description" or "Comment"
 * (case-sensitive; keyword is the first null-terminated string).
 */
static bool gimg_png_text_keyword_is_description_or_comment(
    const unsigned char * payload, size_t payload_size) {
  if (!payload || payload_size == 0) {
    return false;
  }
  size_t kw_len = 0;
  while (kw_len < payload_size && payload[kw_len] != 0) {
    kw_len++;
  }
  if (kw_len >= payload_size) {
    return false;
  }
  if (kw_len == 11 && memcmp(payload, "Description", 11) == 0) {
    return true;
  }
  if (kw_len == 7 && memcmp(payload, "Comment", 7) == 0) {
    return true;
  }
  return false;
}

/**
 * Return true if the text chunk payload has a GPS-related keyword (tEXt/zTXt/iTXt:
 * keyword is the first null-terminated string). STRIP_GPS skips such chunks.
 */
static bool gimg_png_text_keyword_is_gps(
    const unsigned char * payload, size_t payload_size) {
  if (!payload || payload_size == 0) {
    return false;
  }
  size_t kw_len = 0;
  while (kw_len < payload_size && payload[kw_len] != 0) {
    kw_len++;
  }
  if (kw_len == 0) {
    return false;
  }
  // Case-insensitive: "GPS", "GPS ", "EXIF:GPS", "exif:gps", etc.
  if (kw_len >= 3) {
    unsigned char a = (unsigned char)tolower((unsigned char)payload[0]);
    unsigned char b = (unsigned char)tolower((unsigned char)payload[1]);
    unsigned char c = (unsigned char)tolower((unsigned char)payload[2]);
    if (a == 'g' && b == 'p' && c == 's') {
      if (kw_len == 3 || payload[3] == ' ' || payload[3] == 0) {
        return true;
      }
    }
  }
  if (kw_len >= 8) {
    const char * exif_gps = "exif:gps";
    bool match = true;
    for (size_t i = 0; i < 8 && i < kw_len; i++) {
      if (tolower((unsigned char)payload[i]) != (unsigned char)exif_gps[i]) {
        match = false;
        break;
      }
    }
    if (match) {
      return true;
    }
  }
  return false;
}

/** Write a 16-bit sample (host order) to buffer in PNG big-endian order. */
static void gimg_png_write_be16(unsigned char * out, uint16_t value) {
  out[0] = (unsigned char)(value >> 8);
  out[1] = (unsigned char)(value & 0xFFu);
}

/** Fill raw image rows (filter byte + row data) from raster. Caller allocates
 * raw_size = height * (1 + row_bytes). For palette (color_type 3), @a state
 * must be non-NULL with plte/trns; raster must be RGBA8. */
static GIMG_Result gimg_png_raster_to_raw_rows(const GIMG_Raster * raster,
    uint8_t color_type, uint8_t bit_depth, const gimg_png_doc_state_t * state,
    unsigned char * raw, size_t raw_size) {
  uint32_t w = gimg_raster_width(raster);
  uint32_t h = gimg_raster_height(raster);
  size_t row_bytes = gimg_png_row_bytes(color_type, bit_depth, w);
  if (row_bytes == 0) {
    return GIMG_ERR_FORMAT;
  }
  if (raw_size < (size_t)h * (1u + row_bytes)) {
    return GIMG_ERR_INTERNAL;
  }
  size_t stride = gimg_raster_stride_bytes(raster);
  const unsigned char * pixels =
      (const unsigned char *)gimg_raster_pixels_const(raster);

  if (color_type == 3) {
    if (!state || !state->plte || state->plte_size == 0) {
      return GIMG_ERR_FORMAT;
    }
    size_t plte_entries = state->plte_size / 3u;
    size_t trns_count = state->trns ? state->trns_size : 0;
    for (uint32_t y = 0; y < h; y++) {
      unsigned char * row = raw + (size_t)y * (1u + row_bytes);
      row[0] = 0;
      const unsigned char * src = pixels + (size_t)y * stride;
      for (uint32_t x = 0; x < w; x++) {
        unsigned char r = src[0], g = src[1], b = src[2], a = src[3];
        size_t idx = (size_t)-1;
        for (size_t i = 0; i < plte_entries; i++) {
          if (state->plte[i * 3u] != r || state->plte[i * 3u + 1u] != g ||
              state->plte[i * 3u + 2u] != b) {
            continue;
          }
          unsigned char want_a =
              (i < trns_count) ? state->trns[i] : (unsigned char)255;
          if (a != want_a) {
            continue;
          }
          idx = i;
          break;
        }
        if (idx == (size_t)-1) {
          return GIMG_ERR_UNSUPPORTED;
        }
        row[1u + (size_t)x] = (unsigned char)idx;
        src += 4;
      }
    }
    return GIMG_OK;
  }

  for (uint32_t y = 0; y < h; y++) {
    unsigned char * row = raw + (size_t)y * (1u + row_bytes);
    row[0] = 0;
    const unsigned char * src = pixels + (size_t)y * stride;
    if (color_type == 0) {
      if (bit_depth == 8) {
        memcpy(row + 1, src, row_bytes);
      }
      else {
        for (uint32_t x = 0; x < w; x++) {
          uint16_t v = (uint16_t)(src[0] | (src[1] << 8));
          gimg_png_write_be16(row + 1 + (size_t)x * 2u, v);
          src += 2;
        }
      }
    }
    else if (color_type == 2) {
      if (bit_depth == 8) {
        for (uint32_t x = 0; x < w; x++) {
          row[1u + (size_t)x * 3u + 0u] = src[0];
          row[1u + (size_t)x * 3u + 1u] = src[1];
          row[1u + (size_t)x * 3u + 2u] = src[2];
          src += 4;
        }
      }
      else {
        for (uint32_t x = 0; x < w; x++) {
          gimg_png_write_be16(row + 1 + 6u * (size_t)x + 0u,
              (uint16_t)(src[0] | (src[1] << 8)));
          gimg_png_write_be16(row + 1 + 6u * (size_t)x + 2u,
              (uint16_t)(src[2] | (src[3] << 8)));
          gimg_png_write_be16(row + 1 + 6u * (size_t)x + 4u,
              (uint16_t)(src[4] | (src[5] << 8)));
          src += 8;
        }
      }
    }
    else if (color_type == 4) {
      if (bit_depth == 8) {
        for (uint32_t x = 0; x < w; x++) {
          row[1u + (size_t)x * 2u + 0u] = src[0];
          row[1u + (size_t)x * 2u + 1u] = src[3];
          src += 4;
        }
      }
      else {
        for (uint32_t x = 0; x < w; x++) {
          gimg_png_write_be16(row + 1 + 4u * (size_t)x + 0u,
              (uint16_t)(src[0] | (src[1] << 8)));
          gimg_png_write_be16(row + 1 + 4u * (size_t)x + 2u,
              (uint16_t)(src[6] | (src[7] << 8)));
          src += 8;
        }
      }
    }
    else if (color_type == 6) {
      if (bit_depth == 8) {
        memcpy(row + 1, src, row_bytes);
      }
      else {
        for (uint32_t x = 0; x < w; x++) {
          gimg_png_write_be16(row + 1 + 8u * (size_t)x + 0u,
              (uint16_t)(src[0] | (src[1] << 8)));
          gimg_png_write_be16(row + 1 + 8u * (size_t)x + 2u,
              (uint16_t)(src[2] | (src[3] << 8)));
          gimg_png_write_be16(row + 1 + 8u * (size_t)x + 4u,
              (uint16_t)(src[4] | (src[5] << 8)));
          gimg_png_write_be16(row + 1 + 8u * (size_t)x + 6u,
              (uint16_t)(src[6] | (src[7] << 8)));
          src += 8;
        }
      }
    }
  }
  return GIMG_OK;
}

/**
 * Write one pixel from raster at (x,y) to dest in PNG sample order (BE for
 * 16-bit). Returns number of bytes written (1/2 for gray, 1 for palette, 4/8
 * for RGBA).
 */
static size_t gimg_png_write_pixel_at(const GIMG_Raster * raster,
    uint8_t color_type, uint8_t bit_depth, const gimg_png_doc_state_t * state,
    uint32_t x, uint32_t y, unsigned char * dest) {
  size_t stride = gimg_raster_stride_bytes(raster);
  const unsigned char * pixels =
      (const unsigned char *)gimg_raster_pixels_const(raster);
  const unsigned char * src = pixels + (size_t)y * stride;

  if (color_type == 3 && state && state->plte && state->plte_size > 0) {
    size_t plte_entries = state->plte_size / 3u;
    size_t trns_count = state->trns ? state->trns_size : 0;
    const unsigned char * p = src + (size_t)x * 4u;
    unsigned char r = p[0], g = p[1], b = p[2], a = p[3];
    for (size_t i = 0; i < plte_entries; i++) {
      if (state->plte[i * 3u] != r || state->plte[i * 3u + 1u] != g ||
          state->plte[i * 3u + 2u] != b) {
        continue;
      }
      unsigned char want_a =
          (i < trns_count) ? state->trns[i] : (unsigned char)255;
      if (a != want_a) {
        continue;
      }
      dest[0] = (unsigned char)i;
      return 1;
    }
    return 0;  // no palette match
  }

  if (color_type == 0) {
    if (bit_depth == 8) {
      dest[0] = src[x];
      return 1;
    }
    uint16_t v = (uint16_t)(src[x * 2u] | (src[x * 2u + 1u] << 8));
    gimg_png_write_be16(dest, v);
    return 2;
  }
  if (color_type == 2) {
    const unsigned char * p = src + (size_t)x * (bit_depth == 8 ? 4u : 8u);
    if (bit_depth == 8) {
      dest[0] = p[0];
      dest[1] = p[1];
      dest[2] = p[2];
      return 3;
    }
    gimg_png_write_be16(dest + 0, (uint16_t)(p[0] | (p[1] << 8)));
    gimg_png_write_be16(dest + 2, (uint16_t)(p[2] | (p[3] << 8)));
    gimg_png_write_be16(dest + 4, (uint16_t)(p[4] | (p[5] << 8)));
    return 6;
  }
  if (color_type == 4) {
    const unsigned char * p = src + (size_t)x * (bit_depth == 8 ? 4u : 8u);
    if (bit_depth == 8) {
      dest[0] = p[0];
      dest[1] = p[3];
      return 2;
    }
    gimg_png_write_be16(dest + 0, (uint16_t)(p[0] | (p[1] << 8)));
    gimg_png_write_be16(dest + 2, (uint16_t)(p[6] | (p[7] << 8)));
    return 4;
  }
  if (color_type == 6) {
    if (bit_depth == 8) {
      memcpy(dest, src + (size_t)x * 4u, 4);
      return 4;
    }
    const unsigned char * p = src + (size_t)x * 8u;
    gimg_png_write_be16(dest + 0, (uint16_t)(p[0] | (p[1] << 8)));
    gimg_png_write_be16(dest + 2, (uint16_t)(p[2] | (p[3] << 8)));
    gimg_png_write_be16(dest + 4, (uint16_t)(p[4] | (p[5] << 8)));
    gimg_png_write_be16(dest + 6, (uint16_t)(p[6] | (p[7] << 8)));
    return 8;
  }
  return 0;
}

/** Fill raw buffer with Adam7 pass-ordered rows (filter byte + row per pass
 * row). */
static GIMG_Result gimg_png_raster_to_raw_rows_adam7(const GIMG_Raster * raster,
    uint8_t color_type, uint8_t bit_depth, const gimg_png_doc_state_t * state,
    unsigned char * raw, size_t raw_size) {
  uint32_t w = gimg_raster_width(raster);
  uint32_t h = gimg_raster_height(raster);
  size_t expected = 0;
  if (!gimg_png_adam7_raw_size(w, h, color_type, bit_depth, &expected)) {
    return GIMG_ERR_LIMIT;
  }
  if (raw_size < expected) {
    return GIMG_ERR_INTERNAL;
  }
  size_t raw_off = 0;
  for (int pass = 0; pass < 7; pass++) {
    uint32_t pw = 0;
    uint32_t ph = 0;
    gimg_png_adam7_pass_dims(w, h, (unsigned int)pass, &pw, &ph);
    if (pw == 0 || ph == 0) {
      continue;
    }
    const gimg_png_adam7_pass_t * ap = &gimg_png_adam7_passes[pass];

    for (uint32_t j = 0; j < ph; j++) {
      raw[raw_off++] = 0;  // filter byte
      for (uint32_t i = 0; i < pw; i++) {
        uint32_t x = ap->x_offset + i * ap->x_step;
        uint32_t y = ap->y_offset + j * ap->y_step;
        size_t n = gimg_png_write_pixel_at(
            raster, color_type, bit_depth, state, x, y, raw + raw_off);
        if (n == 0) {
          return GIMG_ERR_UNSUPPORTED;
        }
        raw_off += n;
      }
    }
  }
  return GIMG_OK;
}

/**
 * Encode one raster to zlib-wrapped DEFLATE (same format as IDAT/fdAT).
 * On success, *out_zlib is allocated and must be freed by caller.
 */
static GIMG_Result gimg_png_raster_to_zlib(const GIMG_Raster * raster,
    uint8_t color_type, uint8_t bit_depth, const gimg_png_doc_state_t * state,
    int do_interlaced, const GIMG_Allocator * allocator,
    unsigned char ** out_zlib, size_t * out_zlib_len) {
  uint32_t width = gimg_raster_width(raster);
  uint32_t height = gimg_raster_height(raster);
  size_t row_bytes = gimg_png_row_bytes(color_type, bit_depth, width);
  if (row_bytes == 0) {
    return GIMG_ERR_FORMAT;
  }
  size_t raw_size = 0;
  if (do_interlaced) {
    if (!gimg_png_adam7_raw_size(
            width, height, color_type, bit_depth, &raw_size)) {
      return GIMG_ERR_LIMIT;
    }
  }
  else {
    size_t row_stride = 1u + row_bytes;
    if (!gcu_safe_mul_size((size_t)height, row_stride, &raw_size)) {
      return GIMG_ERR_LIMIT;
    }
  }
  unsigned char * raw =
      (unsigned char *)gimg_malloc(gimg_alloc_or_default(allocator), raw_size);
  if (!raw) {
    return GIMG_ERR_OOM;
  }
  GIMG_Result r;
  if (do_interlaced) {
    r = gimg_png_raster_to_raw_rows_adam7(
        raster, color_type, bit_depth, state, raw, raw_size);
  }
  else {
    r = gimg_png_raster_to_raw_rows(
        raster, color_type, bit_depth, state, raw, raw_size);
  }
  if (r != GIMG_OK) {
    gimg_free(gimg_alloc_or_default(allocator), raw);
    return r;
  }
  size_t deflate_cap = raw_size + (raw_size / 2) + 64;
  if (deflate_cap < raw_size) {
    gimg_free(gimg_alloc_or_default(allocator), raw);
    return GIMG_ERR_OOM;
  }
  unsigned char * deflate_buf = (unsigned char *)gimg_malloc(
      gimg_alloc_or_default(allocator), deflate_cap);
  if (!deflate_buf) {
    gimg_free(gimg_alloc_or_default(allocator), raw);
    return GIMG_ERR_OOM;
  }
  gcomp_options_t * gopts = NULL;
  gcomp_status_t gs = gcomp_options_create(&gopts);
  if (gs != GCOMP_OK || !gopts) {
    gimg_free(gimg_alloc_or_default(allocator), deflate_buf);
    gimg_free(gimg_alloc_or_default(allocator), raw);
    return GIMG_ERR_OOM;
  }
  gs = gcomp_options_set_string(gopts, "deflate.strategy", "filtered");
  if (gs != GCOMP_OK) {
    gcomp_options_destroy(gopts);
    gimg_free(gimg_alloc_or_default(allocator), deflate_buf);
    gimg_free(gimg_alloc_or_default(allocator), raw);
    return GIMG_ERR_INTERNAL;
  }
  size_t deflate_len = 0;
  gs = gcomp_encode_buffer(gcomp_registry_default(), "deflate", gopts, raw,
      raw_size, deflate_buf, deflate_cap, &deflate_len);
  gcomp_options_destroy(gopts);
  uint32_t adler = gimg_png_adler32(raw, raw_size);
  gimg_free(gimg_alloc_or_default(allocator), raw);
  if (gs != GCOMP_OK) {
    gimg_free(gimg_alloc_or_default(allocator), deflate_buf);
    if (gs == GCOMP_ERR_MEMORY) {
      return GIMG_ERR_OOM;
    }
    if (gs == GCOMP_ERR_LIMIT) {
      return GIMG_ERR_LIMIT;
    }
    return GIMG_ERR_FORMAT;
  }
  size_t zlib_len = 2 + deflate_len + 4;
  unsigned char * zlib_buf =
      (unsigned char *)gimg_malloc(gimg_alloc_or_default(allocator), zlib_len);
  if (!zlib_buf) {
    gimg_free(gimg_alloc_or_default(allocator), deflate_buf);
    return GIMG_ERR_OOM;
  }
  zlib_buf[0] = 0x78;
  zlib_buf[1] = 0x9C;
  memcpy(zlib_buf + 2, deflate_buf, deflate_len);
  gimg_free(gimg_alloc_or_default(allocator), deflate_buf);
  zlib_buf[zlib_len - 4] = (unsigned char)(adler >> 24);
  zlib_buf[zlib_len - 3] = (unsigned char)(adler >> 16);
  zlib_buf[zlib_len - 2] = (unsigned char)(adler >> 8);
  zlib_buf[zlib_len - 1] = (unsigned char)(adler & 0xFF);
  *out_zlib = zlib_buf;
  *out_zlib_len = zlib_len;
  return GIMG_OK;
}

GIMG_Result gimg_png_save(GIMG_Codec * codec, const GIMG_Doc * doc,
    GIMG_Stream * stream, const char * format_name,
    const GIMG_Save_Options * options, GIMG_Save_Report * report) {
  (void)format_name;
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
  // Prefer attached raster (e.g. load -> modify -> set_raster -> save); else decode
  // or raster from synthetic doc. When attached, doc owns it; when from decode we own.
  GIMG_Raster * raster = NULL;
  int raster_owned = 0;
  GIMG_Result r;
  raster = gimg_item_raster(item);
  if (raster) {
    raster_owned = 0;
  }
  else {
    r = gimg_item_decode(item, NULL, &raster);
    if (r == GIMG_OK && raster) {
      raster_owned = 1;
    }
    else if (r == GIMG_ERR_UNSUPPORTED) {
      return GIMG_ERR_FORMAT;
    }
    else {
      return r != GIMG_OK ? r : GIMG_ERR_FORMAT;
    }
  }
  gimg_png_doc_state_t * state = (gimg_png_doc_state_t *)doc->codec_private;
  GIMG_Color_Info color_info_for_save;
  const GIMG_Color_Info * rci = gimg_raster_color_info_const(raster);
  if (rci) {
    color_info_for_save = *rci;
  }
  else {
    gimg_color_info_default(&color_info_for_save);
  }
  uint8_t color_type = 0;
  uint8_t bit_depth = 0;
  bool use_palette = false;
  if (state && state->plte && state->plte_size > 0 &&
      state->ihdr.color_type == 3) {
    const GIMG_Pixel_Format * fmt = gimg_raster_format(raster);
    if (fmt && fmt->channel_model == GIMG_CHANNEL_RGBA &&
        fmt->channel_count == 4 && fmt->bits_per_channel[0] == 8) {
      use_palette = true;
      color_type = 3;
      bit_depth = state->ihdr.bit_depth;
      if (bit_depth != 1 && bit_depth != 2 && bit_depth != 4 &&
          bit_depth != 8) {
        bit_depth = 8;
      }
    }
  }
  if (!use_palette &&
      !gimg_png_raster_to_ihdr(raster, state, &color_type, &bit_depth)) {
    if (raster_owned) {
      gimg_raster_destroy(raster);
    }
    return GIMG_ERR_UNSUPPORTED;
  }
  uint32_t width = gimg_raster_width(raster);
  uint32_t height = gimg_raster_height(raster);
  int do_interlaced = options && options->interlaced;
  size_t num_items = gimg_doc_item_count(doc);
  int is_apng = (num_items > 1);

  // Encode frame 0 raster to zlib (used for IDAT or first APNG frame).
  unsigned char * zlib_buf = NULL;
  size_t zlib_len = 0;
  r = gimg_png_raster_to_zlib(raster, color_type, bit_depth,
      use_palette ? state : NULL, do_interlaced, codec->allocator, &zlib_buf,
      &zlib_len);
  if (raster_owned) {
    gimg_raster_destroy(raster);
  }
  raster = NULL;
  if (r != GIMG_OK) {
    return r;
  }

  // Write signature.
  size_t n = 0;
  r = gimg_stream_write(stream, gimg_png_signature, GIMG_PNG_SIGNATURE_LEN, &n);
  if (r != GIMG_OK || n != GIMG_PNG_SIGNATURE_LEN) {
    gimg_free(gimg_alloc_or_default(codec->allocator), zlib_buf);
    return r != GIMG_OK ? r : GIMG_ERR_IO;
  }
  report->bytes_written += GIMG_PNG_SIGNATURE_LEN;

  // IHDR
  unsigned char ihdr[GIMG_PNG_IHDR_LEN];
  gimg_png_build_ihdr(ihdr, width, height, bit_depth, color_type,
      (uint8_t)(do_interlaced ? 1 : 0));
  r = gimg_png_write_chunk(stream, GIMG_PNG_IHDR, ihdr, sizeof(ihdr));
  if (r != GIMG_OK) {
    gimg_free(gimg_alloc_or_default(codec->allocator), zlib_buf);
    return r;
  }
  report->bytes_written += 8 + GIMG_PNG_IHDR_LEN + 4;

  // Ancillary before IDAT (per metadata policy).
  GIMG_Meta_Policy policy =
      options ? options->metadata_policy : GIMG_META_PRESERVE_ALL;

  if (policy == GIMG_META_KEEP_COMMON_ONLY) {
    // Emit only color-related metadata from raster's color info.
    if (color_info_for_save.transfer == GIMG_TRANSFER_SRGB ||
        color_info_for_save.primaries == GIMG_PRIMARIES_SRGB) {
      unsigned char srgb_byte =
          (unsigned char)(color_info_for_save.intent & 3u);
      r = gimg_png_write_chunk(stream, GIMG_PNG_sRGB, &srgb_byte, 1);
      if (r != GIMG_OK) {
        gimg_free(gimg_alloc_or_default(codec->allocator), zlib_buf);
        return r;
      }
      report->bytes_written += 8 + 1 + 4;
    }
    else if (color_info_for_save.transfer == GIMG_TRANSFER_GAMMA &&
        color_info_for_save.gamma_value > 0.0) {
      uint32_t gama_val =
          (uint32_t)(color_info_for_save.gamma_value * 100000.0 + 0.5);
      if (gama_val > 0) {
        unsigned char gama[4];
        gama[0] = (unsigned char)(gama_val >> 24);
        gama[1] = (unsigned char)(gama_val >> 16);
        gama[2] = (unsigned char)(gama_val >> 8);
        gama[3] = (unsigned char)(gama_val & 0xFFu);
        r = gimg_png_write_chunk(stream, GIMG_PNG_gAMA, gama, 4);
        if (r != GIMG_OK) {
          gimg_free(gimg_alloc_or_default(codec->allocator), zlib_buf);
          return r;
        }
        report->bytes_written += 8 + 4 + 4;
      }
    }
    else if (color_info_for_save.icc_bytes &&
        color_info_for_save.icc_size > 0) {
      size_t icc_cap = color_info_for_save.icc_size +
          (color_info_for_save.icc_size / 2) + 64;
      unsigned char * icc_compressed = (unsigned char *)gimg_malloc(
          gimg_alloc_or_default(codec->allocator), icc_cap);
      if (icc_compressed) {
        gcomp_options_t * gopts_icc = NULL;
        gcomp_status_t gs = gcomp_options_create(&gopts_icc);
        if (gs == GCOMP_OK && gopts_icc) {
          size_t icc_len = 0;
          gs = gcomp_encode_buffer(gcomp_registry_default(), "deflate",
              gopts_icc, (const unsigned char *)color_info_for_save.icc_bytes,
              color_info_for_save.icc_size, icc_compressed, icc_cap, &icc_len);
          gcomp_options_destroy(gopts_icc);
          if (gs == GCOMP_OK && icc_len > 0) {
            uint32_t adler_icc = gimg_png_adler32(
                (const unsigned char *)color_info_for_save.icc_bytes,
                color_info_for_save.icc_size);
            size_t iccp_len =
                19 + icc_len;  // "ICC Profile\0" + comp byte + zlib
            unsigned char * iccp_buf = (unsigned char *)gimg_malloc(
                gimg_alloc_or_default(codec->allocator), iccp_len);
            if (iccp_buf) {
              memcpy(iccp_buf, "ICC Profile", 11);
              iccp_buf[11] = 0;
              iccp_buf[12] = 0;  // compression method
              iccp_buf[13] = 0x78;
              iccp_buf[14] = 0x9C;
              memcpy(iccp_buf + 15, icc_compressed, icc_len);
              iccp_buf[15 + icc_len] = (unsigned char)(adler_icc >> 24);
              iccp_buf[16 + icc_len] = (unsigned char)(adler_icc >> 16);
              iccp_buf[17 + icc_len] = (unsigned char)(adler_icc >> 8);
              iccp_buf[18 + icc_len] = (unsigned char)(adler_icc & 0xFFu);
              r = gimg_png_write_chunk(
                  stream, GIMG_PNG_iCCP, iccp_buf, 19 + icc_len);
              gimg_free(gimg_alloc_or_default(codec->allocator), iccp_buf);
              if (r == GIMG_OK) {
                report->bytes_written += 8 + (19 + icc_len) + 4;
              }
            }
          }
        }
        gimg_free(gimg_alloc_or_default(codec->allocator), icc_compressed);
      }
      if (r != GIMG_OK) {
        gimg_free(gimg_alloc_or_default(codec->allocator), zlib_buf);
        return r;
      }
    }
  }
  else if (policy != GIMG_META_DROP_ALL && policy != GIMG_META_KEEP_RAW_ONLY) {
    const GIMG_Allocator * alloc = gimg_alloc_or_default(codec->allocator);
    GIMG_Meta_Common * meta_common = gimg_doc_meta_common(doc);
    bool have_description_or_comment_from_ancillary = false;
    // PRESERVE_ALL, STRIP_GPS, or NORMALIZE_EXIF: emit ancillary from state.
    if (state && state->ancillary) {
      for (size_t i = 0; i < state->ancillary_count; i++) {
        gimg_png_chunk_type_t t = state->ancillary[i].type;
        if (t == GIMG_PNG_PLTE || t == GIMG_PNG_tRNS) {
          continue;
        }
        if (t == GIMG_PNG_IHDR || t == GIMG_PNG_IDAT || t == GIMG_PNG_IEND) {
          continue;
        }
        if (policy == GIMG_META_STRIP_GPS &&
            (t == GIMG_PNG_tEXt || t == GIMG_PNG_zTXt || t == GIMG_PNG_iTXt) &&
            state->ancillary[i].payload &&
            state->ancillary[i].payload_size > 0) {
          if (gimg_png_text_keyword_is_gps(state->ancillary[i].payload,
                  state->ancillary[i].payload_size)) {
            continue;
          }
        }
        if ((t == GIMG_PNG_tEXt || t == GIMG_PNG_zTXt || t == GIMG_PNG_iTXt) &&
            state->ancillary[i].payload && state->ancillary[i].payload_size > 0 &&
            gimg_png_text_keyword_is_description_or_comment(
                state->ancillary[i].payload, state->ancillary[i].payload_size)) {
          have_description_or_comment_from_ancillary = true;
        }
        const void * chunk_payload = state->ancillary[i].payload;
        size_t chunk_size = state->ancillary[i].payload_size;
        void * modified = NULL;
        size_t modified_size = 0;
        if (t == GIMG_PNG_eXIf && chunk_payload && chunk_size > 0) {
          if (policy == GIMG_META_STRIP_GPS) {
            if (gimg_exif_strip_gps(codec->allocator, chunk_payload,
                    chunk_size, &modified, &modified_size) == GIMG_OK) {
              chunk_payload = modified;
              chunk_size = modified_size;
            }
          }
          else if (policy == GIMG_META_NORMALIZE_EXIF) {
            if (gimg_exif_normalize(codec->allocator, chunk_payload,
                    chunk_size, &modified, &modified_size) == GIMG_OK) {
              chunk_payload = modified;
              chunk_size = modified_size;
            }
          }
        }
        r = gimg_png_write_chunk(stream, t, chunk_payload, chunk_size);
        if (modified) {
          gimg_free(alloc, modified);
        }
        if (r != GIMG_OK) {
          gimg_free(alloc, zlib_buf);
          return r;
        }
        report->bytes_written += 8 + chunk_size + 4;
      }
    }
    // If meta_common has description and we did not write one from ancillary,
    // emit one tEXt "Description\0" + description.
    if (!have_description_or_comment_from_ancillary && meta_common) {
      const char * desc = gimg_meta_common_description(meta_common);
      if (desc) {
        size_t dlen = strlen(desc);
        size_t kw_len = 11;  // "Description"
        if (dlen <= 0x7FFFFFFFu - kw_len - 1u) {
          size_t total = kw_len + 1u + dlen;
          unsigned char * tEXt_payload =
              (unsigned char *)gimg_malloc(alloc, total);
          if (tEXt_payload) {
            memcpy(tEXt_payload, "Description", 11);
            tEXt_payload[11] = 0;
            memcpy(tEXt_payload + 12, desc, dlen);
            r = gimg_png_write_chunk(
                stream, GIMG_PNG_tEXt, tEXt_payload, total);
            gimg_free(alloc, tEXt_payload);
            if (r == GIMG_OK) {
              report->bytes_written += 8 + total + 4;
            }
            else {
              gimg_free(alloc, zlib_buf);
              return r;
            }
          }
        }
      }
    }
    // eXIf from doc meta_raw when doc was not loaded from PNG (no state).
    if (!state) {
      GIMG_Meta_Raw * meta_raw = gimg_doc_meta_raw(doc);
      if (meta_raw) {
        size_t exif_size = 0;
        r = gimg_meta_raw_get(
            meta_raw, "png", (uint32_t)GIMG_PNG_eXIf, NULL, &exif_size);
        if (r == GIMG_OK && exif_size > 0) {
          unsigned char * exif_buf =
              (unsigned char *)gimg_malloc(alloc, exif_size);
          if (exif_buf) {
            r = gimg_meta_raw_get(
                meta_raw, "png", (uint32_t)GIMG_PNG_eXIf, exif_buf, &exif_size);
            if (r == GIMG_OK) {
              void * to_write = exif_buf;
              size_t to_write_size = exif_size;
              void * modified = NULL;
              size_t modified_size = 0;
              if (policy == GIMG_META_STRIP_GPS) {
                if (gimg_exif_strip_gps(codec->allocator, exif_buf, exif_size,
                        &modified, &modified_size) == GIMG_OK) {
                  to_write = modified;
                  to_write_size = modified_size;
                }
              }
              else if (policy == GIMG_META_NORMALIZE_EXIF) {
                if (gimg_exif_normalize(codec->allocator, exif_buf, exif_size,
                        &modified, &modified_size) == GIMG_OK) {
                  to_write = modified;
                  to_write_size = modified_size;
                }
              }
              r = gimg_png_write_chunk(
                  stream, GIMG_PNG_eXIf, to_write, to_write_size);
              report->bytes_written += 8 + to_write_size + 4;
              if (modified) {
                gimg_free(alloc, modified);
              }
            }
            gimg_free(alloc, exif_buf);
          }
          if (r != GIMG_OK) {
            gimg_free(alloc, zlib_buf);
            return r;
          }
        }
      }
    }
  }
  else if (policy == GIMG_META_KEEP_RAW_ONLY && state && state->ancillary) {
    // Emit only ancillary chunks that are not known semantic (raw/unknown
    // only).
    for (size_t i = 0; i < state->ancillary_count; i++) {
      gimg_png_chunk_type_t t = state->ancillary[i].type;
      if (t == GIMG_PNG_PLTE || t == GIMG_PNG_tRNS) {
        continue;
      }
      if (t == GIMG_PNG_IHDR || t == GIMG_PNG_IDAT || t == GIMG_PNG_IEND) {
        continue;
      }
      if (gimg_png_chunk_is_known_semantic(t)) {
        continue;
      }
      r = gimg_png_write_chunk(stream, state->ancillary[i].type,
          state->ancillary[i].payload, state->ancillary[i].payload_size);
      if (r != GIMG_OK) {
        gimg_free(gimg_alloc_or_default(codec->allocator), zlib_buf);
        return r;
      }
      report->bytes_written += 8 + state->ancillary[i].payload_size + 4;
    }
  }
  // DROP_ALL: no ancillary (already skipped above).

  // Palette: PLTE and tRNS before IDAT per PNG spec.
  if (color_type == 3 && state && state->plte && state->plte_size > 0) {
    r = gimg_png_write_chunk(
        stream, GIMG_PNG_PLTE, state->plte, state->plte_size);
    if (r != GIMG_OK) {
      gimg_free(gimg_alloc_or_default(codec->allocator), zlib_buf);
      return r;
    }
    report->bytes_written += 8 + state->plte_size + 4;
    if (state->trns && state->trns_size > 0) {
      r = gimg_png_write_chunk(
          stream, GIMG_PNG_tRNS, state->trns, state->trns_size);
      if (r != GIMG_OK) {
        gimg_free(gimg_alloc_or_default(codec->allocator), zlib_buf);
        return r;
      }
      report->bytes_written += 8 + state->trns_size + 4;
    }
  }

  // APNG: acTL (num_frames, num_plays) before first fcTL per spec.
  if (is_apng) {
    uint32_t num_plays = (state && state->is_apng) ? state->num_plays : 0u;
    unsigned char actl[GIMG_PNG_acTL_LEN];
    gimg_png_build_actl(actl, (uint32_t)num_items, num_plays);
    r = gimg_png_write_chunk(stream, GIMG_PNG_acTL, actl, sizeof(actl));
    if (r != GIMG_OK) {
      gimg_free(gimg_alloc_or_default(codec->allocator), zlib_buf);
      return r;
    }
    report->bytes_written += 8 + GIMG_PNG_acTL_LEN + 4;
  }

  // fcTL for frame 0 (APNG only).
  if (is_apng) {
    GIMG_Item * frame_item = gimg_doc_item((GIMG_Doc *)doc, 0);
    uint16_t delay_num = 0, delay_den = 0;
    gimg_item_frame_delay(frame_item, &delay_num, &delay_den);
    if (delay_den == 0) {
      delay_den = 100;
    }
    uint8_t dispose_op = (uint8_t)gimg_item_dispose_op(frame_item);
    uint8_t blend_op = (uint8_t)gimg_item_blend_op(frame_item);
    unsigned char fctl[GIMG_PNG_fcTL_LEN];
    gimg_png_build_fctl(fctl, 0u, width, height, 0u, 0u, delay_num, delay_den,
        dispose_op, blend_op);
    r = gimg_png_write_chunk(stream, GIMG_PNG_fcTL, fctl, sizeof(fctl));
    if (r != GIMG_OK) {
      gimg_free(gimg_alloc_or_default(codec->allocator), zlib_buf);
      return r;
    }
    report->bytes_written += 8 + GIMG_PNG_fcTL_LEN + 4;
  }

  // IDAT (frame 0 image data; single or multiple chunks when > 32 KiB).
  {
    size_t idat_chunk_max = GIMG_PNG_IDAT_CHUNK_MAX;
    size_t idat_offset = 0;
    while (idat_offset < zlib_len) {
      size_t chunk_len = zlib_len - idat_offset;
      if (chunk_len > idat_chunk_max) {
        chunk_len = idat_chunk_max;
      }
      r = gimg_png_write_chunk(
          stream, GIMG_PNG_IDAT, zlib_buf + idat_offset, chunk_len);
      if (r != GIMG_OK) {
        gimg_free(gimg_alloc_or_default(codec->allocator), zlib_buf);
        return r;
      }
      report->bytes_written += 8 + chunk_len + 4;
      idat_offset += chunk_len;
    }
  }
  gimg_free(gimg_alloc_or_default(codec->allocator), zlib_buf);
  zlib_buf = NULL;

  // APNG: fcTL + fdAT for frames 1 .. N-1. Sequence numbers are a single
  // increasing run: fcTL 0, fcTL 1, fdAT 2, fdAT 3, ... (see APNG spec).
  for (size_t frame_index = 1; frame_index < num_items && is_apng;
       frame_index++) {
    GIMG_Item * frame_item = gimg_doc_item((GIMG_Doc *)doc, frame_index);
    if (!frame_item) {
      return GIMG_ERR_INTERNAL;
    }
    GIMG_Raster * frame_raster = NULL;
    int frame_raster_owned = 0;
    frame_raster = gimg_item_raster(frame_item);
    if (frame_raster) {
      frame_raster_owned = 0;
    }
    else {
      r = gimg_item_decode(frame_item, NULL, &frame_raster);
      if (r == GIMG_OK && frame_raster) {
        frame_raster_owned = 1;
      }
      else if (r == GIMG_ERR_UNSUPPORTED) {
        return GIMG_ERR_FORMAT;
      }
      else {
        return r != GIMG_OK ? r : GIMG_ERR_FORMAT;
      }
    }
    if (gimg_raster_width(frame_raster) != width ||
        gimg_raster_height(frame_raster) != height) {
      if (frame_raster_owned) {
        gimg_raster_destroy(frame_raster);
      }
      return GIMG_ERR_FORMAT;
    }
    if (!use_palette) {
      uint8_t ct = 0;
      uint8_t bd = 0;
      if (!gimg_png_raster_to_ihdr(frame_raster, state, &ct, &bd) ||
          ct != color_type || bd != bit_depth) {
        if (frame_raster_owned) {
          gimg_raster_destroy(frame_raster);
        }
        return GIMG_ERR_FORMAT;
      }
    }
    uint16_t delay_num = 0, delay_den = 0;
    gimg_item_frame_delay(frame_item, &delay_num, &delay_den);
    if (delay_den == 0) {
      delay_den = 100;
    }
    uint8_t dispose_op = (uint8_t)gimg_item_dispose_op(frame_item);
    uint8_t blend_op = (uint8_t)gimg_item_blend_op(frame_item);
    // APNG: one global sequence (no duplicates). fcTL(0), fcTL(1), fdAT(2),
    // fcTL(3), fdAT(4), ... so fcTL for frame_index has seq 2*frame_index-1.
    uint32_t fctl_sequence = (uint32_t)(2u * frame_index - 1u);
    unsigned char fctl[GIMG_PNG_fcTL_LEN];
    gimg_png_build_fctl(fctl, fctl_sequence, width, height, 0u, 0u,
        delay_num, delay_den, dispose_op, blend_op);
    r = gimg_png_write_chunk(stream, GIMG_PNG_fcTL, fctl, sizeof(fctl));
    if (r != GIMG_OK) {
      if (frame_raster_owned) {
        gimg_raster_destroy(frame_raster);
      }
      return r;
    }
    report->bytes_written += 8 + GIMG_PNG_fcTL_LEN + 4;

    unsigned char * frame_zlib = NULL;
    size_t frame_zlib_len = 0;
    r = gimg_png_raster_to_zlib(frame_raster, color_type, bit_depth,
        use_palette ? state : NULL, do_interlaced, codec->allocator,
        &frame_zlib, &frame_zlib_len);
    if (frame_raster_owned) {
      gimg_raster_destroy(frame_raster);
    }
    if (r != GIMG_OK) {
      return r;
    }
    // fdAT: first chunk has sequence 2*frame_index, then increment (APNG).
    uint32_t fdat_sequence = (uint32_t)(2u * frame_index);
    size_t fdat_chunk_max = GIMG_PNG_IDAT_CHUNK_MAX;
    size_t fdat_offset = 0;
    while (fdat_offset < frame_zlib_len) {
      size_t frag_len = frame_zlib_len - fdat_offset;
      if (frag_len > fdat_chunk_max) {
        frag_len = fdat_chunk_max;
      }
      size_t payload_len = 4u + frag_len;
      unsigned char * fdat_payload = (unsigned char *)gimg_malloc(
          gimg_alloc_or_default(codec->allocator), payload_len);
      if (!fdat_payload) {
        gimg_free(gimg_alloc_or_default(codec->allocator), frame_zlib);
        return GIMG_ERR_OOM;
      }
      fdat_payload[0] = (unsigned char)(fdat_sequence >> 24);
      fdat_payload[1] = (unsigned char)(fdat_sequence >> 16);
      fdat_payload[2] = (unsigned char)(fdat_sequence >> 8);
      fdat_payload[3] = (unsigned char)(fdat_sequence & 0xFFu);
      memcpy(fdat_payload + 4, frame_zlib + fdat_offset, frag_len);
      r = gimg_png_write_chunk(
          stream, GIMG_PNG_fdAT, fdat_payload, payload_len);
      gimg_free(gimg_alloc_or_default(codec->allocator), fdat_payload);
      if (r != GIMG_OK) {
        gimg_free(gimg_alloc_or_default(codec->allocator), frame_zlib);
        return r;
      }
      report->bytes_written += 8 + payload_len + 4;
      fdat_offset += frag_len;
      fdat_sequence++;
    }
    gimg_free(gimg_alloc_or_default(codec->allocator), frame_zlib);
  }

  // IEND
  r = gimg_png_write_chunk(stream, GIMG_PNG_IEND, NULL, 0);
  if (r != GIMG_OK) {
    return r;
  }
  report->bytes_written += 12;
  return GIMG_OK;
}

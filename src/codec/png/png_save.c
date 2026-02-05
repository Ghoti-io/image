/**
 * @file
 *
 * PNG save: encode raster to PNG (signature, IHDR, ancillary, IDAT, IEND).
 * Uses DEFLATE via compress library; zlib-wraps for IDAT (2-byte header +
 * raw DEFLATE + 4-byte Adler-32 per RFC 1950).
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <ghoti.io/image/codec.h>
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
 * doc state for that).
 */
static int gimg_png_raster_to_ihdr(const GIMG_Raster * raster,
    uint8_t * color_type, uint8_t * bit_depth) {
  const GIMG_Pixel_Format * fmt = gimg_raster_format(raster);
  if (!fmt || fmt->layout != GIMG_LAYOUT_INTERLEAVED) {
    return 0;
  }
  if (fmt->channel_model == GIMG_CHANNEL_GRAY && fmt->channel_count >= 1) {
    if (fmt->bits_per_channel[0] == 8) {
      *color_type = 0;
      *bit_depth = 8;
      return 1;
    }
    if (fmt->bits_per_channel[0] == 16) {
      *color_type = 0;
      *bit_depth = 16;
      return 1;
    }
    return 0;
  }
  if (fmt->channel_model == GIMG_CHANNEL_RGBA && fmt->channel_count == 4) {
    if (fmt->bits_per_channel[0] == 8) {
      *color_type = 6;
      *bit_depth = 8;
      return 1;
    }
    if (fmt->bits_per_channel[0] == 16) {
      *color_type = 6;
      *bit_depth = 16;
      return 1;
    }
    return 0;
  }
  return 0;
}

/** Build IHDR payload (13 bytes). */
static void gimg_png_build_ihdr(unsigned char * out,
    uint32_t width, uint32_t height, uint8_t bit_depth, uint8_t color_type) {
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
  out[10] = 0;  // compression_method
  out[11] = 0;  // filter_method
  out[12] = 0;  // interlace_method
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
  size_t row_bytes = 0;
  switch (color_type) {
  case 0:
    row_bytes = (size_t)w * (bit_depth == 16 ? 2u : 1u);
    break;
  case 3:
    row_bytes = (size_t)w;
    break;
  case 6:
    row_bytes = (size_t)w * (bit_depth == 16 ? 8u : 4u);
    break;
  default:
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
          unsigned char want_a = (i < trns_count) ? state->trns[i] : (unsigned char)255;
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
  GIMG_Raster * raster = NULL;
  GIMG_Result r = gimg_item_decode(item, NULL, &raster);
  if (r != GIMG_OK || !raster) {
    return r != GIMG_OK ? r : GIMG_ERR_FORMAT;
  }
  gimg_png_doc_state_t * state =
      (gimg_png_doc_state_t *)doc->codec_private;
  uint8_t color_type = 0;
  uint8_t bit_depth = 0;
  int use_palette = 0;
  if (state && state->plte && state->plte_size > 0 && state->ihdr.color_type == 3) {
    const GIMG_Pixel_Format * fmt = gimg_raster_format(raster);
    if (fmt && fmt->channel_model == GIMG_CHANNEL_RGBA &&
        fmt->channel_count == 4 && fmt->bits_per_channel[0] == 8) {
      use_palette = 1;
      color_type = 3;
      bit_depth = state->ihdr.bit_depth;
      if (bit_depth != 1 && bit_depth != 2 && bit_depth != 4 && bit_depth != 8) {
        bit_depth = 8;
      }
    }
  }
  if (!use_palette && !gimg_png_raster_to_ihdr(raster, &color_type, &bit_depth)) {
    gimg_raster_destroy(raster);
    return GIMG_ERR_UNSUPPORTED;
  }
  uint32_t width = gimg_raster_width(raster);
  uint32_t height = gimg_raster_height(raster);
  size_t row_bytes;
  if (color_type == 0) {
    row_bytes = (size_t)width * (bit_depth == 16 ? 2u : 1u);
  }
  else if (color_type == 3) {
    row_bytes = (size_t)width;
  }
  else {
    row_bytes = (size_t)width * (bit_depth == 16 ? 8u : 4u);
  }
  size_t raw_size = (size_t)height * (1u + row_bytes);
  unsigned char * raw = (unsigned char *)gimg_malloc(
      gimg_alloc_or_default(codec->allocator), raw_size);
  if (!raw) {
    gimg_raster_destroy(raster);
    return GIMG_ERR_OOM;
  }
  r = gimg_png_raster_to_raw_rows(raster, color_type, bit_depth,
      use_palette ? state : NULL, raw, raw_size);
  gimg_raster_destroy(raster);
  if (r != GIMG_OK) {
    gimg_free(gimg_alloc_or_default(codec->allocator), raw);
    return r;
  }

  // DEFLATE compress (raw = filtered rows).
  size_t deflate_cap = raw_size + (raw_size / 2) + 64;
  if (deflate_cap < raw_size) {
    gimg_free(gimg_alloc_or_default(codec->allocator), raw);
    return GIMG_ERR_OOM;
  }
  unsigned char * deflate_buf = (unsigned char *)gimg_malloc(
      gimg_alloc_or_default(codec->allocator), deflate_cap);
  if (!deflate_buf) {
    gimg_free(gimg_alloc_or_default(codec->allocator), raw);
    return GIMG_ERR_OOM;
  }
  gcomp_options_t * gopts = NULL;
  gcomp_status_t gs = gcomp_options_create(&gopts);
  if (gs != GCOMP_OK || !gopts) {
    gimg_free(gimg_alloc_or_default(codec->allocator), deflate_buf);
    gimg_free(gimg_alloc_or_default(codec->allocator), raw);
    return GIMG_ERR_OOM;
  }
  gs = gcomp_options_set_string(gopts, "deflate.strategy", "filtered");
  if (gs != GCOMP_OK) {
    gcomp_options_destroy(gopts);
    gimg_free(gimg_alloc_or_default(codec->allocator), deflate_buf);
    gimg_free(gimg_alloc_or_default(codec->allocator), raw);
    return GIMG_ERR_INTERNAL;
  }
  size_t deflate_len = 0;
  gs = gcomp_encode_buffer(gcomp_registry_default(), "deflate", gopts,
      raw, raw_size, deflate_buf, deflate_cap, &deflate_len);
  gcomp_options_destroy(gopts);
  if (gs != GCOMP_OK) {
    gimg_free(gimg_alloc_or_default(codec->allocator), deflate_buf);
    gimg_free(gimg_alloc_or_default(codec->allocator), raw);
    return gs == GCOMP_ERR_MEMORY ? GIMG_ERR_OOM : GIMG_ERR_FORMAT;
  }
  uint32_t adler = gimg_png_adler32(raw, raw_size);
  gimg_free(gimg_alloc_or_default(codec->allocator), raw);

  // Zlib wrap: 2-byte header + raw deflate + 4-byte Adler-32 (BE).
  size_t zlib_len = 2 + deflate_len + 4;
  unsigned char * zlib_buf = (unsigned char *)gimg_malloc(
      gimg_alloc_or_default(codec->allocator), zlib_len);
  if (!zlib_buf) {
    gimg_free(gimg_alloc_or_default(codec->allocator), deflate_buf);
    return GIMG_ERR_OOM;
  }
  zlib_buf[0] = 0x78;
  zlib_buf[1] = 0x9C;
  memcpy(zlib_buf + 2, deflate_buf, deflate_len);
  zlib_buf[zlib_len - 4] = (unsigned char)(adler >> 24);
  zlib_buf[zlib_len - 3] = (unsigned char)(adler >> 16);
  zlib_buf[zlib_len - 2] = (unsigned char)(adler >> 8);
  zlib_buf[zlib_len - 1] = (unsigned char)(adler & 0xFF);
  gimg_free(gimg_alloc_or_default(codec->allocator), deflate_buf);

  // Write signature.
  size_t n = 0;
  r = gimg_stream_write(stream, gimg_png_signature, GIMG_PNG_SIGNATURE_LEN,
      &n);
  if (r != GIMG_OK || n != GIMG_PNG_SIGNATURE_LEN) {
    gimg_free(gimg_alloc_or_default(codec->allocator), zlib_buf);
    return r != GIMG_OK ? r : GIMG_ERR_IO;
  }
  report->bytes_written += GIMG_PNG_SIGNATURE_LEN;

  // IHDR
  unsigned char ihdr[GIMG_PNG_IHDR_LEN];
  gimg_png_build_ihdr(ihdr, width, height, bit_depth, color_type);
  r = gimg_png_write_chunk(stream, GIMG_PNG_IHDR, ihdr, sizeof(ihdr));
  if (r != GIMG_OK) {
    gimg_free(gimg_alloc_or_default(codec->allocator), zlib_buf);
    return r;
  }
  report->bytes_written += 8 + GIMG_PNG_IHDR_LEN + 4;

  // Ancillary before IDAT (per policy). Preserve from codec_private if PNG-loaded.
  GIMG_Meta_Policy policy = options ? options->metadata_policy : GIMG_META_PRESERVE_ALL;
  if (policy == GIMG_META_PRESERVE_ALL && state && state->ancillary) {
    for (size_t i = 0; i < state->ancillary_count; i++) {
      gimg_png_chunk_type_t t = state->ancillary[i].type;
      if (t == GIMG_PNG_PLTE || t == GIMG_PNG_tRNS) {
        continue;
      }
      if (t == GIMG_PNG_IHDR || t == GIMG_PNG_IDAT || t == GIMG_PNG_IEND) {
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
  // eXIf from doc meta_raw when doc was not loaded from PNG (no state).
  if (policy == GIMG_META_PRESERVE_ALL && !state) {
    GIMG_Meta_Raw * meta_raw = gimg_doc_meta_raw(doc);
    if (meta_raw) {
      size_t exif_size = 0;
      r = gimg_meta_raw_get(meta_raw, "png", (uint32_t)GIMG_PNG_eXIf, NULL,
          &exif_size);
      if (r == GIMG_OK && exif_size > 0) {
        unsigned char * exif_buf = (unsigned char *)gimg_malloc(
            gimg_alloc_or_default(codec->allocator), exif_size);
        if (exif_buf) {
          r = gimg_meta_raw_get(meta_raw, "png", (uint32_t)GIMG_PNG_eXIf,
              exif_buf, &exif_size);
          if (r == GIMG_OK) {
            r = gimg_png_write_chunk(stream, GIMG_PNG_eXIf, exif_buf,
                exif_size);
            report->bytes_written += 8 + exif_size + 4;
          }
          gimg_free(gimg_alloc_or_default(codec->allocator), exif_buf);
        }
        if (r != GIMG_OK) {
          gimg_free(gimg_alloc_or_default(codec->allocator), zlib_buf);
          return r;
        }
      }
    }
  }

  // Palette: PLTE and tRNS before IDAT per PNG spec.
  if (color_type == 3 && state && state->plte && state->plte_size > 0) {
    r = gimg_png_write_chunk(stream, GIMG_PNG_PLTE, state->plte,
        state->plte_size);
    if (r != GIMG_OK) {
      gimg_free(gimg_alloc_or_default(codec->allocator), zlib_buf);
      return r;
    }
    report->bytes_written += 8 + state->plte_size + 4;
    if (state->trns && state->trns_size > 0) {
      r = gimg_png_write_chunk(stream, GIMG_PNG_tRNS, state->trns,
          state->trns_size);
      if (r != GIMG_OK) {
        gimg_free(gimg_alloc_or_default(codec->allocator), zlib_buf);
        return r;
      }
      report->bytes_written += 8 + state->trns_size + 4;
    }
  }

  // IDAT
  r = gimg_png_write_chunk(stream, GIMG_PNG_IDAT, zlib_buf, zlib_len);
  gimg_free(gimg_alloc_or_default(codec->allocator), zlib_buf);
  if (r != GIMG_OK) {
    return r;
  }
  report->bytes_written += 8 + zlib_len + 4;

  // IEND
  r = gimg_png_write_chunk(stream, GIMG_PNG_IEND, NULL, 0);
  if (r != GIMG_OK) {
    return r;
  }
  report->bytes_written += 12;
  return GIMG_OK;
}

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
#include <ghoti.io/image/color.h>
#include <ghoti.io/image/doc.h>
#include <ghoti.io/image/meta.h>
#include <ghoti.io/image/raster.h>
#include <ghoti.io/image/stream.h>
#include <stddef.h>
#include <stdint.h>
#include <ctype.h>
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

/** Build IHDR payload (13 bytes). @a interlace_method 0 or 1 (Adam7). */
static void gimg_png_build_ihdr(unsigned char * out,
    uint32_t width, uint32_t height, uint8_t bit_depth, uint8_t color_type,
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
  out[10] = 0;  // compression_method
  out[11] = 0;  // filter_method
  out[12] = interlace_method;
}

/** Adam7 pass parameters (W3C §2.6). */
typedef struct {
  unsigned int x_offset;
  unsigned int y_offset;
  unsigned int x_step;
  unsigned int y_step;
} gimg_png_adam7_pass_t;

static const gimg_png_adam7_pass_t gimg_png_adam7_passes[7] = {
    {0, 0, 8, 8},
    {4, 0, 8, 8},
    {0, 4, 4, 8},
    {2, 0, 4, 4},
    {0, 2, 2, 4},
    {1, 0, 2, 2},
    {0, 1, 1, 2},
};

static void gimg_png_adam7_pass_dims(uint32_t image_width,
    uint32_t image_height, unsigned int pass_index, uint32_t * out_pw,
    uint32_t * out_ph) {
  const gimg_png_adam7_pass_t * p = &gimg_png_adam7_passes[pass_index];
  *out_pw = (image_width > p->x_offset)
      ? (uint32_t)((image_width - p->x_offset + p->x_step - 1) / p->x_step)
      : 0;
  *out_ph = (image_height > p->y_offset)
      ? (uint32_t)((image_height - p->y_offset + p->y_step - 1) / p->y_step)
      : 0;
}

/** Row bytes for a given width (excluding filter byte). */
static size_t gimg_png_row_bytes_for_ct_bd(
    uint8_t color_type, uint8_t bit_depth, uint32_t w) {
  size_t samples = 0;
  switch (color_type) {
  case 0:
    samples = (size_t)w * (bit_depth == 16 ? 2u : 1u);
    break;
  case 3:
    samples = (size_t)w;
    break;
  case 6:
    samples = (size_t)w * (bit_depth == 16 ? 8u : 4u);
    break;
  default:
    return 0;
  }
  return samples;
}

/** Expected raw size for Adam7 (filter byte + row per pass row). */
static size_t gimg_png_adam7_raw_size(uint32_t width, uint32_t height,
    uint8_t color_type, uint8_t bit_depth) {
  size_t total = 0;
  for (int pass = 0; pass < 7; pass++) {
    uint32_t pw = 0;
    uint32_t ph = 0;
    gimg_png_adam7_pass_dims(width, height, (unsigned int)pass, &pw, &ph);
    if (pw == 0 || ph == 0) {
      continue;
    }
    size_t row_bytes = gimg_png_row_bytes_for_ct_bd(color_type, bit_depth, pw);
    total += (size_t)ph * (1u + row_bytes);
  }
  return total;
}

/** Return 1 if chunk type is known semantic metadata (color, Exif, text). */
static int gimg_png_chunk_is_known_semantic(gimg_png_chunk_type_t t) {
  return t == GIMG_PNG_iCCP || t == GIMG_PNG_sRGB || t == GIMG_PNG_gAMA ||
      t == GIMG_PNG_cHRM || t == GIMG_PNG_eXIf || t == GIMG_PNG_tEXt ||
      t == GIMG_PNG_zTXt || t == GIMG_PNG_iTXt;
}

/**
 * Return 1 if the text chunk payload has a GPS-related keyword (tEXt/zTXt/iTXt:
 * keyword is the first null-terminated string). STRIP_GPS skips such chunks.
 */
static int gimg_png_text_keyword_is_gps(const unsigned char * payload,
    size_t payload_size) {
  if (!payload || payload_size == 0) {
    return 0;
  }
  size_t kw_len = 0;
  while (kw_len < payload_size && payload[kw_len] != 0) {
    kw_len++;
  }
  if (kw_len == 0) {
    return 0;
  }
  /* Case-insensitive: "GPS", "GPS ", "EXIF:GPS", "exif:gps", etc. */
  if (kw_len >= 3) {
    unsigned char a = (unsigned char)tolower((unsigned char)payload[0]);
    unsigned char b = (unsigned char)tolower((unsigned char)payload[1]);
    unsigned char c = (unsigned char)tolower((unsigned char)payload[2]);
    if (a == 'g' && b == 'p' && c == 's') {
      if (kw_len == 3 || payload[3] == ' ' || payload[3] == 0) {
        return 1;
      }
    }
  }
  if (kw_len >= 8) {
    const char * exif_gps = "exif:gps";
    int match = 1;
    for (size_t i = 0; i < 8 && i < kw_len; i++) {
      if (tolower((unsigned char)payload[i]) != (unsigned char)exif_gps[i]) {
        match = 0;
        break;
      }
    }
    if (match) {
      return 1;
    }
  }
  return 0;
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

/**
 * Write one pixel from raster at (x,y) to dest in PNG sample order (BE for 16-bit).
 * Returns number of bytes written (1/2 for gray, 1 for palette, 4/8 for RGBA).
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
      unsigned char want_a = (i < trns_count) ? state->trns[i] : (unsigned char)255;
      if (a != want_a) {
        continue;
      }
      dest[0] = (unsigned char)i;
      return 1;
    }
    return 0;  /* no palette match */
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

/** Fill raw buffer with Adam7 pass-ordered rows (filter byte + row per pass row). */
static GIMG_Result gimg_png_raster_to_raw_rows_adam7(const GIMG_Raster * raster,
    uint8_t color_type, uint8_t bit_depth, const gimg_png_doc_state_t * state,
    unsigned char * raw, size_t raw_size) {
  uint32_t w = gimg_raster_width(raster);
  uint32_t h = gimg_raster_height(raster);
  size_t expected = gimg_png_adam7_raw_size(w, h, color_type, bit_depth);
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
      raw[raw_off++] = 0;  /* filter byte */
      for (uint32_t i = 0; i < pw; i++) {
        uint32_t x = ap->x_offset + i * ap->x_step;
        uint32_t y = ap->y_offset + j * ap->y_step;
        size_t n = gimg_png_write_pixel_at(raster, color_type, bit_depth,
            state, x, y, raw + raw_off);
        if (n == 0) {
          return GIMG_ERR_UNSUPPORTED;
        }
        raw_off += n;
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
  int do_interlaced = options && options->interlaced;
  size_t raw_size;
  if (do_interlaced) {
    raw_size = gimg_png_adam7_raw_size(width, height, color_type, bit_depth);
  }
  else {
    raw_size = (size_t)height * (1u + row_bytes);
  }
  unsigned char * raw = (unsigned char *)gimg_malloc(
      gimg_alloc_or_default(codec->allocator), raw_size);
  if (!raw) {
    gimg_raster_destroy(raster);
    return GIMG_ERR_OOM;
  }
  if (do_interlaced) {
    r = gimg_png_raster_to_raw_rows_adam7(raster, color_type, bit_depth,
        use_palette ? state : NULL, raw, raw_size);
  }
  else {
    r = gimg_png_raster_to_raw_rows(raster, color_type, bit_depth,
        use_palette ? state : NULL, raw, raw_size);
  }
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
  gimg_png_build_ihdr(ihdr, width, height, bit_depth, color_type,
      (uint8_t)(do_interlaced ? 1 : 0));
  r = gimg_png_write_chunk(stream, GIMG_PNG_IHDR, ihdr, sizeof(ihdr));
  if (r != GIMG_OK) {
    gimg_free(gimg_alloc_or_default(codec->allocator), zlib_buf);
    return r;
  }
  report->bytes_written += 8 + GIMG_PNG_IHDR_LEN + 4;

  // Ancillary before IDAT (per metadata policy).
  GIMG_Meta_Policy policy = options ? options->metadata_policy : GIMG_META_PRESERVE_ALL;

  if (policy == GIMG_META_KEEP_COMMON_ONLY) {
    // Emit only color-related metadata from raster's color info.
    if (color_info_for_save.transfer == GIMG_TRANSFER_SRGB ||
        color_info_for_save.primaries == GIMG_PRIMARIES_SRGB) {
      unsigned char srgb_byte = (unsigned char)(color_info_for_save.intent & 3u);
      r = gimg_png_write_chunk(stream, GIMG_PNG_sRGB, &srgb_byte, 1);
      if (r != GIMG_OK) {
        gimg_free(gimg_alloc_or_default(codec->allocator), zlib_buf);
        return r;
      }
      report->bytes_written += 8 + 1 + 4;
    }
    else if (color_info_for_save.transfer == GIMG_TRANSFER_GAMMA &&
        color_info_for_save.gamma_value > 0.0) {
      uint32_t gama_val = (uint32_t)(color_info_for_save.gamma_value * 100000.0 + 0.5);
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
    else if (color_info_for_save.icc_bytes && color_info_for_save.icc_size > 0) {
      size_t icc_cap = color_info_for_save.icc_size + (color_info_for_save.icc_size / 2) + 64;
      unsigned char * icc_compressed = (unsigned char *)gimg_malloc(
          gimg_alloc_or_default(codec->allocator), icc_cap);
      if (icc_compressed) {
        gcomp_options_t * gopts_icc = NULL;
        gcomp_status_t gs = gcomp_options_create(&gopts_icc);
        if (gs == GCOMP_OK && gopts_icc) {
          size_t icc_len = 0;
          gs = gcomp_encode_buffer(gcomp_registry_default(), "deflate", gopts_icc,
              (const unsigned char *)color_info_for_save.icc_bytes,
              color_info_for_save.icc_size, icc_compressed, icc_cap, &icc_len);
          gcomp_options_destroy(gopts_icc);
          if (gs == GCOMP_OK && icc_len > 0) {
            uint32_t adler_icc = gimg_png_adler32(
                (const unsigned char *)color_info_for_save.icc_bytes,
                color_info_for_save.icc_size);
            size_t iccp_len = 19 + icc_len;  /* "ICC Profile\0" + comp byte + zlib */
            unsigned char * iccp_buf = (unsigned char *)gimg_malloc(
                gimg_alloc_or_default(codec->allocator), iccp_len);
            if (iccp_buf) {
              memcpy(iccp_buf, "ICC Profile", 11);
              iccp_buf[11] = 0;
              iccp_buf[12] = 0;  /* compression method */
              iccp_buf[13] = 0x78;
              iccp_buf[14] = 0x9C;
              memcpy(iccp_buf + 15, icc_compressed, icc_len);
              iccp_buf[15 + icc_len] = (unsigned char)(adler_icc >> 24);
              iccp_buf[16 + icc_len] = (unsigned char)(adler_icc >> 16);
              iccp_buf[17 + icc_len] = (unsigned char)(adler_icc >> 8);
              iccp_buf[18 + icc_len] = (unsigned char)(adler_icc & 0xFFu);
              r = gimg_png_write_chunk(stream, GIMG_PNG_iCCP, iccp_buf,
                  19 + icc_len);
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
        if (policy == GIMG_META_STRIP_GPS && t == GIMG_PNG_eXIf) {
          continue;
        }
        if (policy == GIMG_META_STRIP_GPS &&
            (t == GIMG_PNG_tEXt || t == GIMG_PNG_zTXt || t == GIMG_PNG_iTXt) &&
            state->ancillary[i].payload && state->ancillary[i].payload_size > 0) {
          if (gimg_png_text_keyword_is_gps(state->ancillary[i].payload,
              state->ancillary[i].payload_size)) {
            continue;
          }
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
    if (!state && policy != GIMG_META_STRIP_GPS) {
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
  }
  else if (policy == GIMG_META_KEEP_RAW_ONLY && state && state->ancillary) {
    // Emit only ancillary chunks that are not known semantic (raw/unknown only).
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

  // IDAT (single chunk or multiple chunks when payload > 32 KiB)
  {
    size_t idat_chunk_max = 32768u;
    size_t idat_offset = 0;
    while (idat_offset < zlib_len) {
      size_t chunk_len = zlib_len - idat_offset;
      if (chunk_len > idat_chunk_max) {
        chunk_len = idat_chunk_max;
      }
      r = gimg_png_write_chunk(stream, GIMG_PNG_IDAT,
          zlib_buf + idat_offset, chunk_len);
      if (r != GIMG_OK) {
        gimg_free(gimg_alloc_or_default(codec->allocator), zlib_buf);
        return r;
      }
      report->bytes_written += 8 + chunk_len + 4;
      idat_offset += chunk_len;
    }
  }
  gimg_free(gimg_alloc_or_default(codec->allocator), zlib_buf);

  // IEND
  r = gimg_png_write_chunk(stream, GIMG_PNG_IEND, NULL, 0);
  if (r != GIMG_OK) {
    return r;
  }
  report->bytes_written += 12;
  return GIMG_OK;
}

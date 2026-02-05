/**
 * @file
 *
 * PNG decode: DEFLATE decompress IDAT, apply filters, produce raster.
 *
 * Specification references:
 * - W3C PNG: https://www.w3.org/TR/PNG/ (Recommendation 10 Nov 2003)
 * - W3C Data representation (filtering, interlace):
 * https://www.w3.org/TR/PNG-DataRep.html
 * - W3C Filter algorithms: https://www.w3.org/TR/PNG-Filters.html
 * - ISO/IEC 15948:2004 (PNG — Portable Network Graphics)
 * - Interlaced data order (Adam7): W3C §2.6
 *   https://www.w3.org/TR/PNG-DataRep.html#DR.Interlaced-data-order
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <ghoti.io/image/codec.h>
#include <ghoti.io/image/color.h>
#include <ghoti.io/image/doc.h>
#include <ghoti.io/image/raster.h>
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

/** Max decompressed ICC profile size (bomb protection). */
#define GIMG_PNG_ICC_MAX_DECODED (4u * 1024u * 1024u)

/** Map compress status to image result (for decode path). */
static GIMG_Result gimg_png_result_from_gcomp(gcomp_status_t s) {
  switch (s) {
  case GCOMP_OK:
    return GIMG_OK;
  case GCOMP_ERR_MEMORY:
    return GIMG_ERR_OOM;
  case GCOMP_ERR_LIMIT:
    return GIMG_ERR_LIMIT;
  case GCOMP_ERR_CORRUPT:
    return GIMG_ERR_CORRUPT;
  case GCOMP_ERR_IO:
    return GIMG_ERR_IO;
  default:
    return GIMG_ERR_FORMAT;
  }
}

/**
 * Color chunk policy (PNG allows at most one of sRGB, iCCP, or gAMA+cHRM).
 * We use first in priority order: sRGB > iCCP > gAMA/cHRM.
 * Fills @a out_info; for iCCP allocates decompressed profile and sets
 * @a out_icc_owned (caller frees). Returns 1 if color info was set, 0 if none.
 */
static int gimg_png_fill_color_info_from_ancillary(
    const gimg_png_doc_state_t * state, const GIMG_Allocator * alloc,
    GIMG_Color_Info * out_info, void ** out_icc_owned, size_t * out_icc_size) {
  gimg_color_info_default(out_info);
  *out_icc_owned = NULL;
  *out_icc_size = 0;

  size_t first_srgb = (size_t)-1, first_iccp = (size_t)-1, first_gama = (size_t)-1;
  for (size_t i = 0; i < state->ancillary_count; i++) {
    gimg_png_chunk_type_t t = state->ancillary[i].type;
    if (t == GIMG_PNG_sRGB && first_srgb == (size_t)-1) {
      first_srgb = i;
    }
    else if (t == GIMG_PNG_iCCP && first_iccp == (size_t)-1) {
      first_iccp = i;
    }
    else if (t == GIMG_PNG_gAMA && first_gama == (size_t)-1) {
      first_gama = i;
    }
  }

  // Priority: sRGB > iCCP > gAMA.
  if (first_srgb != (size_t)-1) {
    const unsigned char * p = state->ancillary[first_srgb].payload;
    size_t len = state->ancillary[first_srgb].payload_size;
    if (len >= 1) {
      unsigned int intent = (unsigned int)p[0];
      if (intent > 3) {
        intent = 0;
      }
      out_info->primaries = GIMG_PRIMARIES_SRGB;
      out_info->white_point = GIMG_PRIMARIES_SRGB;
      out_info->transfer = GIMG_TRANSFER_SRGB;
      out_info->intent = (GIMG_Rendering_Intent)intent;
      return 1;
    }
  }
  if (first_iccp != (size_t)-1) {
    const unsigned char * payload = state->ancillary[first_iccp].payload;
    size_t payload_len = state->ancillary[first_iccp].payload_size;
    const unsigned char * name_end =
        (const unsigned char *)memchr(payload, 0, payload_len);
    if (name_end && name_end - payload + 2 < (ptrdiff_t)payload_len) {
      size_t name_len = (size_t)(name_end - payload);
      uint8_t comp = payload[name_len + 1];
      const unsigned char * zlib_start = payload + name_len + 2;
      size_t zlib_len = payload_len - name_len - 2;
      if (comp == 0 && zlib_len > 6) {
        const unsigned char * deflate_src = zlib_start + 2;
        size_t deflate_len = zlib_len - 6;
        size_t max_out = GIMG_PNG_ICC_MAX_DECODED;
        void * decoded = gimg_malloc(alloc, max_out);
        if (!decoded) {
          return 0;
        }
        size_t out_len = 0;
        gcomp_options_t * gopts = NULL;
        gcomp_status_t gs = gcomp_options_create(&gopts);
        if (gs != GCOMP_OK || !gopts) {
          gimg_free(alloc, decoded);
          return 0;
        }
        gs = gcomp_options_set_uint64(gopts, "limits.max_output_bytes", max_out);
        if (gs != GCOMP_OK) {
          gcomp_options_destroy(gopts);
          gimg_free(alloc, decoded);
          return 0;
        }
        gs = gcomp_decode_buffer(gcomp_registry_default(), "deflate", gopts,
            deflate_src, deflate_len, decoded, max_out, &out_len);
        gcomp_options_destroy(gopts);
        if (gs != GCOMP_OK) {
          gimg_free(alloc, decoded);
          return 0;
        }
        out_info->primaries = GIMG_PRIMARIES_UNKNOWN;
        out_info->white_point = GIMG_PRIMARIES_UNKNOWN;
        out_info->transfer = GIMG_TRANSFER_UNKNOWN;
        out_info->icc_bytes = decoded;
        out_info->icc_size = out_len;
        *out_icc_owned = decoded;
        *out_icc_size = out_len;
        return 1;
      }
    }
  }
  if (first_gama != (size_t)-1) {
    const unsigned char * p = state->ancillary[first_gama].payload;
    size_t len = state->ancillary[first_gama].payload_size;
    if (len >= 4) {
      uint32_t gama_val = (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 |
          (uint32_t)p[2] << 8 | (uint32_t)p[3];
      if (gama_val > 0) {
        out_info->primaries = GIMG_PRIMARIES_UNKNOWN;
        out_info->white_point = GIMG_PRIMARIES_UNKNOWN;
        out_info->transfer = GIMG_TRANSFER_GAMMA;
        out_info->gamma_value = (double)gama_val / 100000.0;
        return 1;
      }
    }
  }
  return 0;
}

/** Bytes per row for a given pixel width (excluding filter byte). PNG §3.2. */
static size_t gimg_png_row_bytes_for_width(
    const gimg_png_ihdr_t * ihdr, uint32_t width) {
  uint8_t depth = ihdr->bit_depth;
  uint8_t ct = ihdr->color_type;
  size_t samples_per_row = 0;
  switch (ct) {
  case 0:
    samples_per_row = (size_t)width;
    break;
  case 2:
    samples_per_row = (size_t)width * 3;
    break;
  case 3:
    samples_per_row = (size_t)width;
    break;
  case 4:
    samples_per_row = (size_t)width * 2;
    break;
  case 6:
    samples_per_row = (size_t)width * 4;
    break;
  default:
    return 0;
  }
  return (samples_per_row * (size_t)depth + 7) / 8;
}

/** Bytes per row of image data (excluding filter byte). */
static size_t gimg_png_row_bytes(const gimg_png_ihdr_t * ihdr) {
  return gimg_png_row_bytes_for_width(ihdr, ihdr->width);
}

/** Adam7 pass parameters (W3C §2.6 Interlaced data order). */
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

/** Pass width/height for Adam7 (empty pass returns 0). W3C §2.6. */
static void gimg_png_adam7_pass_dims(uint32_t image_width,
    uint32_t image_height, unsigned int pass_index, uint32_t * out_pass_width,
    uint32_t * out_pass_height) {
  const gimg_png_adam7_pass_t * p = &gimg_png_adam7_passes[pass_index];
  *out_pass_width = (image_width > p->x_offset)
      ? (uint32_t)((image_width - p->x_offset + p->x_step - 1) / p->x_step)
      : 0;
  *out_pass_height = (image_height > p->y_offset)
      ? (uint32_t)((image_height - p->y_offset + p->y_step - 1) / p->y_step)
      : 0;
}

/** Expected decompressed size for Adam7: sum over passes of rows × (1 +
 * row_bytes). */
static size_t gimg_png_expected_raw_size_adam7(const gimg_png_ihdr_t * ihdr) {
  uint32_t w = ihdr->width;
  uint32_t h = ihdr->height;
  size_t total = 0;
  for (int pass = 0; pass < 7; pass++) {
    uint32_t pw = 0;
    uint32_t ph = 0;
    gimg_png_adam7_pass_dims(w, h, (unsigned int)pass, &pw, &ph);
    if (pw == 0 || ph == 0) {
      continue;
    }
    size_t row_bytes = gimg_png_row_bytes_for_width(ihdr, pw);
    total += (size_t)ph * (1 + row_bytes);
  }
  return total;
}

/** Expected decompressed size (non-interlaced or Adam7). */
static size_t gimg_png_expected_raw_size(const gimg_png_ihdr_t * ihdr) {
  if (ihdr->interlace_method == 0) {
    size_t row = gimg_png_row_bytes(ihdr);
    if (row == 0) {
      return 0;
    }
    return (size_t)ihdr->height * (1 + row);
  }
  return gimg_png_expected_raw_size_adam7(ihdr);
}

/** Paeth predictor (W3C PNG-Filters §6.6). */
static unsigned char gimg_png_paeth(int a, int b, int c) {
  int p = a + b - c;
  int pa = p >= a ? p - a : a - p;
  int pb = p >= b ? p - b : b - p;
  int pc = p >= c ? p - c : c - p;
  if (pa <= pb && pa <= pc) {
    return (unsigned char)a;
  }
  if (pb <= pc) {
    return (unsigned char)b;
  }
  return (unsigned char)c;
}

/** Scale a sample from 1/2/4-bit to 8-bit (PNG sample range to 0..255). */
static unsigned char gimg_png_scale_to_8(unsigned int sample, uint8_t depth) {
  if (depth >= 8) {
    return (unsigned char)sample;
  }
  unsigned int max_val = (1u << depth) - 1u;
  if (max_val == 0) {
    return (unsigned char)(sample ? 255 : 0);
  }
  return (unsigned char)((sample * 255u + max_val / 2u) / max_val);
}

/** Read 16-bit big-endian sample from buffer. */
static uint16_t gimg_png_read_be16(const unsigned char * p) {
  return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

/**
 * Get one sample (channel) at x from a decoded row. For depth 1/2/4 samples are
 * packed; for 8 one byte; for 16 two bytes big-endian. Returns value in
 * 0..(2^depth - 1) for depth <= 8, or 0..65535 for depth 16.
 */
static uint32_t gimg_png_sample_at(
    const unsigned char * row, uint32_t x, uint8_t depth) {
  if (depth == 8) {
    return (uint32_t)row[x];
  }
  if (depth == 16) {
    return (uint32_t)gimg_png_read_be16(row + (size_t)x * 2u);
  }
  if (depth == 1) {
    size_t byte_ix = (size_t)x / 8u;
    unsigned int bit = 7 - (unsigned int)(x % 8u);
    return (uint32_t)((row[byte_ix] >> bit) & 1u);
  }
  if (depth == 2) {
    size_t byte_ix = (size_t)x / 4u;
    unsigned int shift = 6 - 2u * (unsigned int)(x % 4u);
    return (uint32_t)((row[byte_ix] >> shift) & 3u);
  }
  if (depth == 4) {
    size_t byte_ix = (size_t)x / 2u;
    uint32_t v = (uint32_t)row[byte_ix];
    return (x & 1u) ? (v & 15u) : (v >> 4u);
  }
  return 0;
}

/** Bytes per pixel for raw PNG (bpp for filter). */
static unsigned int gimg_png_bpp(const gimg_png_ihdr_t * ihdr) {
  uint8_t depth = ihdr->bit_depth;
  uint8_t ct = ihdr->color_type;
  unsigned int channels = 0;
  switch (ct) {
  case 0:
    channels = 1;
    break;
  case 2:
    channels = 3;
    break;
  case 3:
    channels = 1;
    break;
  case 4:
    channels = 2;
    break;
  case 6:
    channels = 4;
    break;
  default:
    return 1;
  }
  return (channels * (unsigned int)depth + 7) / 8;
}

/** Unfilter one row; prior is previous row (or NULL for first row). */
static void gimg_png_unfilter_row(unsigned char * row, size_t row_bytes,
    const unsigned char * prior, unsigned int bpp) {
  unsigned char filter = row[0];
  unsigned char * raw = row + 1;
  size_t n = row_bytes;

  switch (filter) {
  case 0: // None
    (void)prior;
    (void)bpp;
    break;
  case 1: // Sub
  {
    (void)prior;
    for (size_t i = 0; i < n; i++) {
      unsigned char left = (i >= (size_t)bpp) ? raw[i - bpp] : 0;
      raw[i] = (unsigned char)((unsigned int)raw[i] + (unsigned int)left);
    }
    break;
  }
  case 2: // Up
  {
    for (size_t i = 0; i < n; i++) {
      unsigned char up = prior ? prior[i] : 0;
      raw[i] = (unsigned char)((unsigned int)raw[i] + (unsigned int)up);
    }
    break;
  }
  case 3: // Average
  {
    for (size_t i = 0; i < n; i++) {
      unsigned char left = (i >= (size_t)bpp) ? raw[i - bpp] : 0;
      unsigned char up = prior ? prior[i] : 0;
      raw[i] = (unsigned char)((unsigned int)raw[i] +
          ((unsigned int)left + (unsigned int)up) / 2);
    }
    break;
  }
  case 4: // Paeth
  {
    for (size_t i = 0; i < n; i++) {
      int a = (i >= (size_t)bpp) ? (int)raw[i - bpp] : 0;
      int b = prior ? (int)prior[i] : 0;
      int c = (prior && i >= (size_t)bpp) ? (int)prior[i - bpp] : 0;
      raw[i] = (unsigned char)((unsigned int)raw[i] +
          (unsigned int)gimg_png_paeth(a, b, c));
    }
    break;
  }
  default:
    break;
  }
}

GIMG_Result gimg_png_decode(GIMG_Codec * codec, const GIMG_Item * item,
    const GIMG_Decode_Options * options, GIMG_Raster ** out_raster) {
  if (!codec || !item || !out_raster) {
    return GIMG_ERR_INTERNAL;
  }
  *out_raster = NULL;

  const GIMG_Doc * doc = item->doc;
  if (!doc || !doc->codec_private) {
    return GIMG_ERR_UNSUPPORTED;
  }
  // Allow decode when the codec that loaded the doc is the same as the one
  // requesting decode (pointer match), or when they match by name (handles
  // save path where codec comes from gimg_codec_by_name and may differ by
  // pointer from doc->loaded_by_codec in some link scenarios).
  const GIMG_Codec * doc_codec = doc->loaded_by_codec;
  if (!doc_codec ||
      (doc_codec != codec &&
       (!gimg_codec_name(doc_codec) || !gimg_codec_name(codec) ||
        strcmp(gimg_codec_name(doc_codec), gimg_codec_name(codec)) != 0))) {
    return GIMG_ERR_UNSUPPORTED;
  }

  gimg_png_doc_state_t * state = (gimg_png_doc_state_t *)doc->codec_private;
  if (item->index >= 1) {
    return GIMG_ERR_UNSUPPORTED; // Single-frame only for now.
  }

  const gimg_png_ihdr_t * ihdr = &state->ihdr;
  if (ihdr->interlace_method > 1) {
    return GIMG_ERR_UNSUPPORTED;
  }

  size_t row_bytes = gimg_png_row_bytes(ihdr);
  size_t raw_size = gimg_png_expected_raw_size(ihdr);
  if (raw_size == 0 || !state->idat) {
    return GIMG_ERR_FORMAT;
  }

  const GIMG_Limits * limits = options ? options->limits : NULL;
  size_t max_pixels = (limits && limits->max_decoded_pixels != 0)
      ? limits->max_decoded_pixels
      : (size_t)ihdr->width * (size_t)ihdr->height;
  if ((size_t)ihdr->width * (size_t)ihdr->height > max_pixels) {
    return GIMG_ERR_LIMIT;
  }

  const GIMG_Allocator * alloc = codec->allocator;
  alloc = gimg_alloc_or_default(alloc);

  gcomp_options_t * gopts = NULL;
  gcomp_status_t gs = gcomp_options_create(&gopts);
  if (gs != GCOMP_OK || !gopts) {
    return gimg_png_result_from_gcomp(gs);
  }
  gs = gcomp_options_set_uint64(gopts, "limits.max_output_bytes", raw_size);
  if (gs != GCOMP_OK) {
    gcomp_options_destroy(gopts);
    return GIMG_ERR_INTERNAL;
  }

  // PNG IDAT is zlib-wrapped (RFC 1950): 2-byte header + raw DEFLATE + 4-byte
  // Adler-32. The compress library "deflate" method expects raw DEFLATE.
  if (state->idat_size < 6) {
    gcomp_options_destroy(gopts);
    return GIMG_ERR_CORRUPT;
  }
  const unsigned char * deflate_src = state->idat + 2;
  size_t deflate_len = state->idat_size - 6;

  unsigned char * raw = (unsigned char *)gimg_malloc(alloc, raw_size);
  if (!raw) {
    gcomp_options_destroy(gopts);
    return GIMG_ERR_OOM;
  }

  size_t out_len = 0;
  gs = gcomp_decode_buffer(gcomp_registry_default(), "deflate", gopts,
      deflate_src, deflate_len, raw, raw_size, &out_len);
  gcomp_options_destroy(gopts);
  if (gs != GCOMP_OK) {
    gimg_free(alloc, raw);
    return gimg_png_result_from_gcomp(gs);
  }
  if (out_len != raw_size) {
    gimg_free(alloc, raw);
    return GIMG_ERR_CORRUPT;
  }

  unsigned int bpp = gimg_png_bpp(ihdr);
  uint32_t w = ihdr->width;
  uint32_t h = ihdr->height;

  // Build unfiltered image in row-major form (no filter bytes) for raster copy.
  size_t raw_full_size = (size_t)h * row_bytes;
  unsigned char * raw_full = (unsigned char *)gimg_malloc(alloc, raw_full_size);
  if (!raw_full) {
    gimg_free(alloc, raw);
    return GIMG_ERR_OOM;
  }

  if (ihdr->interlace_method == 0) {
    size_t row_stride = 1 + row_bytes;
    unsigned char * prev_row = NULL;
    for (uint32_t y = 0; y < h; y++) {
      unsigned char * row = raw + (size_t)y * row_stride;
      gimg_png_unfilter_row(row, row_bytes, prev_row, bpp);
      prev_row = row + 1;
      memcpy(raw_full + (size_t)y * row_bytes, row + 1, row_bytes);
    }
  }
  else {
    // Adam7: each pass is filtered independently; scatter into raw_full.
    size_t raw_off = 0;
    for (int pass = 0; pass < 7; pass++) {
      uint32_t pw = 0;
      uint32_t ph = 0;
      gimg_png_adam7_pass_dims(w, h, (unsigned int)pass, &pw, &ph);
      if (pw == 0 || ph == 0) {
        continue;
      }
      const gimg_png_adam7_pass_t * ap = &gimg_png_adam7_passes[pass];
      size_t pass_row_bytes = gimg_png_row_bytes_for_width(ihdr, pw);
      size_t pass_row_stride = 1 + pass_row_bytes;
      unsigned char * prev_row = NULL;
      for (uint32_t j = 0; j < ph; j++) {
        unsigned char * row = raw + raw_off + (size_t)j * pass_row_stride;
        gimg_png_unfilter_row(row, pass_row_bytes, prev_row, bpp);
        prev_row = row + 1;
        for (uint32_t i = 0; i < pw; i++) {
          uint32_t ix = ap->x_offset + i * ap->x_step;
          uint32_t iy = ap->y_offset + j * ap->y_step;
          size_t dst_off = (size_t)iy * row_bytes + (size_t)ix * bpp;
          size_t src_off = (size_t)i * bpp;
          memcpy(raw_full + dst_off, row + 1 + src_off, (size_t)bpp);
        }
      }
      raw_off += (size_t)ph * pass_row_stride;
    }
  }

  gimg_free(alloc, raw);

  // Select output format and whether we need alpha from tRNS.
  int use_trns = (state->trns && state->trns_size > 0) ? 1 : 0;
  const GIMG_Pixel_Format * format = NULL;
  switch (ihdr->color_type) {
  case 0:
    if (ihdr->bit_depth <= 8) {
      format = use_trns ? &GIMG_PIXEL_RGBA8 : &GIMG_PIXEL_GRAY8;
    }
    else {
      format = use_trns ? &GIMG_PIXEL_RGBA16 : &GIMG_PIXEL_GRAY16;
    }
    break;
  case 2:
    format = (ihdr->bit_depth <= 8) ? &GIMG_PIXEL_RGBA8 : &GIMG_PIXEL_RGBA16;
    break;
  case 3:
    format = &GIMG_PIXEL_RGBA8; // Palette always expanded to RGBA8 (+ tRNS).
    break;
  case 4:
    format = (ihdr->bit_depth <= 8) ? &GIMG_PIXEL_RGBA8 : &GIMG_PIXEL_RGBA16;
    break;
  case 6:
    format = (ihdr->bit_depth <= 8) ? &GIMG_PIXEL_RGBA8 : &GIMG_PIXEL_RGBA16;
    break;
  default:
    gimg_free(alloc, raw_full);
    return GIMG_ERR_FORMAT;
  }

  size_t bpp_out = gimg_raster_bytes_per_pixel(format);
  size_t stride = (size_t)ihdr->width * bpp_out;
  if (stride % GIMG_DEFAULT_STRIDE_ALIGNMENT) {
    stride = (stride + GIMG_DEFAULT_STRIDE_ALIGNMENT - 1) &
        ~(size_t)(GIMG_DEFAULT_STRIDE_ALIGNMENT - 1);
  }
  size_t total = stride * (size_t)ihdr->height;
  void * pixels = gimg_malloc(alloc, total);
  if (!pixels) {
    gimg_free(alloc, raw_full);
    return GIMG_ERR_OOM;
  }

  // Convert raw_full to output raster (all bit depths, tRNS, palette).
  uint8_t depth = ihdr->bit_depth;
  size_t plte_entries = state->plte ? state->plte_size / 3u : 0;

  if (ihdr->color_type == 0) {
    // Grayscale: 1/2/4/8/16-bit; tRNS = 2-byte gray key (BE).
    uint16_t trns_gray = 0;
    if (use_trns && state->trns_size >= 2) {
      trns_gray = gimg_png_read_be16(state->trns);
    }
    if (format == &GIMG_PIXEL_GRAY8) {
      for (uint32_t y = 0; y < h; y++) {
        const unsigned char * src = raw_full + (size_t)y * row_bytes;
        unsigned char * dst = (unsigned char *)pixels + (size_t)y * stride;
        for (uint32_t x = 0; x < w; x++) {
          uint32_t v = gimg_png_sample_at(src, x, depth);
          dst[x] = gimg_png_scale_to_8((unsigned int)v, depth);
        }
      }
    }
    else if (format == &GIMG_PIXEL_GRAY16) {
      for (uint32_t y = 0; y < h; y++) {
        const unsigned char * src = raw_full + (size_t)y * row_bytes;
        unsigned char * dst = (unsigned char *)pixels + (size_t)y * stride;
        for (uint32_t x = 0; x < w; x++) {
          uint32_t v = gimg_png_sample_at(src, x, depth);
          uint16_t le = (uint16_t)v;
          dst[x * 2u] = (unsigned char)(le & 0xFFu);
          dst[x * 2u + 1u] = (unsigned char)(le >> 8);
        }
      }
    }
    else {
      // RGBA8 or RGBA16 with optional tRNS key.
      int has_trns = use_trns && state->trns_size >= 2;
      for (uint32_t y = 0; y < h; y++) {
        const unsigned char * src = raw_full + (size_t)y * row_bytes;
        unsigned char * dst = (unsigned char *)pixels + (size_t)y * stride;
        for (uint32_t x = 0; x < w; x++) {
          uint32_t v = gimg_png_sample_at(src, x, depth);
          unsigned char g8 = depth <= 8
              ? gimg_png_scale_to_8((unsigned int)v, depth)
              : (unsigned char)((v >> 8) & 0xFFu);
          uint16_t v16 = (uint16_t)v;
          int match = has_trns && (v16 == trns_gray);
          if (format == &GIMG_PIXEL_RGBA8) {
            dst[0] = g8;
            dst[1] = g8;
            dst[2] = g8;
            dst[3] = (unsigned char)(match ? 0 : 255);
            dst += 4;
          }
          else {
            uint16_t a16 = (uint16_t)(match ? 0 : 65535);
            dst[0] = (unsigned char)(v16 & 0xFFu);
            dst[1] = (unsigned char)(v16 >> 8);
            dst[2] = (unsigned char)(v16 & 0xFFu);
            dst[3] = (unsigned char)(v16 >> 8);
            dst[4] = (unsigned char)(v16 & 0xFFu);
            dst[5] = (unsigned char)(v16 >> 8);
            dst[6] = (unsigned char)(a16 & 0xFFu);
            dst[7] = (unsigned char)(a16 >> 8);
            dst += 8;
          }
        }
      }
    }
  }
  else if (ihdr->color_type == 2) {
    // RGB: 8 or 16-bit; tRNS = 6 bytes R,G,B key (BE).
    uint16_t trns_r = 0, trns_g = 0, trns_b = 0;
    int has_trns = 0;
    if (use_trns && state->trns_size >= 6) {
      trns_r = gimg_png_read_be16(state->trns);
      trns_g = gimg_png_read_be16(state->trns + 2);
      trns_b = gimg_png_read_be16(state->trns + 4);
      has_trns = 1;
    }
    if (depth == 8) {
      for (uint32_t y = 0; y < h; y++) {
        const unsigned char * src = raw_full + (size_t)y * row_bytes;
        unsigned char * dst = (unsigned char *)pixels + (size_t)y * stride;
        for (uint32_t x = 0; x < w; x++) {
          unsigned char r = src[0], g = src[1], b = src[2];
          int match = has_trns &&
              (r == (trns_r & 0xFF) && g == (trns_g & 0xFF) &&
                  b == (trns_b & 0xFF));
          dst[0] = r;
          dst[1] = g;
          dst[2] = b;
          dst[3] = (unsigned char)(match ? 0 : 255);
          src += 3;
          dst += 4;
        }
      }
    }
    else {
      for (uint32_t y = 0; y < h; y++) {
        const unsigned char * src = raw_full + (size_t)y * row_bytes;
        unsigned char * dst = (unsigned char *)pixels + (size_t)y * stride;
        for (uint32_t x = 0; x < w; x++) {
          uint16_t r = gimg_png_read_be16(src), g = gimg_png_read_be16(src + 2),
                   b = gimg_png_read_be16(src + 4);
          int match = has_trns && (r == trns_r && g == trns_g && b == trns_b);
          dst[0] = (unsigned char)(r & 0xFFu);
          dst[1] = (unsigned char)(r >> 8);
          dst[2] = (unsigned char)(g & 0xFFu);
          dst[3] = (unsigned char)(g >> 8);
          dst[4] = (unsigned char)(b & 0xFFu);
          dst[5] = (unsigned char)(b >> 8);
          dst[6] = (unsigned char)(match ? 0 : 255);
          dst[7] = (unsigned char)(match ? 0 : 255);
          src += 6;
          dst += 8;
        }
      }
    }
  }
  else if (ihdr->color_type == 3) {
    // Palette: 1/2/4/8-bit index; PLTE + optional tRNS (1 byte per entry).
    if (plte_entries == 0) {
      gimg_free(alloc, pixels);
      gimg_free(alloc, raw_full);
      return GIMG_ERR_FORMAT;
    }
    for (uint32_t y = 0; y < h; y++) {
      const unsigned char * src = raw_full + (size_t)y * row_bytes;
      unsigned char * dst = (unsigned char *)pixels + (size_t)y * stride;
      for (uint32_t x = 0; x < w; x++) {
        unsigned int idx = (unsigned int)gimg_png_sample_at(src, x, depth);
        if (idx >= 256) {
          idx = 255;
        }
        size_t off = (size_t)idx * 3u;
        dst[0] = off + 0 < state->plte_size ? state->plte[off + 0] : 0;
        dst[1] = off + 1 < state->plte_size ? state->plte[off + 1] : 0;
        dst[2] = off + 2 < state->plte_size ? state->plte[off + 2] : 0;
        dst[3] = (state->trns && (size_t)idx < state->trns_size)
            ? state->trns[idx]
            : (unsigned char)255;
        dst += 4;
      }
    }
  }
  else if (ihdr->color_type == 4) {
    // Grayscale + alpha: 8 or 16-bit.
    if (depth == 8) {
      for (uint32_t y = 0; y < h; y++) {
        const unsigned char * src = raw_full + (size_t)y * row_bytes;
        unsigned char * dst = (unsigned char *)pixels + (size_t)y * stride;
        for (uint32_t x = 0; x < w; x++) {
          unsigned char g = src[0], a = src[1];
          dst[0] = dst[1] = dst[2] = g;
          dst[3] = a;
          src += 2;
          dst += 4;
        }
      }
    }
    else {
      for (uint32_t y = 0; y < h; y++) {
        const unsigned char * src = raw_full + (size_t)y * row_bytes;
        unsigned char * dst = (unsigned char *)pixels + (size_t)y * stride;
        for (uint32_t x = 0; x < w; x++) {
          uint16_t g = gimg_png_read_be16(src), a = gimg_png_read_be16(src + 2);
          dst[0] = (unsigned char)(g & 0xFFu);
          dst[1] = (unsigned char)(g >> 8);
          dst[2] = (unsigned char)(g & 0xFFu);
          dst[3] = (unsigned char)(g >> 8);
          dst[4] = (unsigned char)(g & 0xFFu);
          dst[5] = (unsigned char)(g >> 8);
          dst[6] = (unsigned char)(a & 0xFFu);
          dst[7] = (unsigned char)(a >> 8);
          src += 4;
          dst += 8;
        }
      }
    }
  }
  else if (ihdr->color_type == 6) {
    // RGBA: 8 or 16-bit.
    if (depth == 8) {
      for (uint32_t y = 0; y < h; y++) {
        const unsigned char * src = raw_full + (size_t)y * row_bytes;
        unsigned char * dst = (unsigned char *)pixels + (size_t)y * stride;
        memcpy(dst, src, (size_t)w * 4u);
      }
    }
    else {
      for (uint32_t y = 0; y < h; y++) {
        const unsigned char * src = raw_full + (size_t)y * row_bytes;
        unsigned char * dst = (unsigned char *)pixels + (size_t)y * stride;
        for (uint32_t x = 0; x < w; x++) {
          dst[0] = (unsigned char)(gimg_png_read_be16(src) & 0xFFu);
          dst[1] = (unsigned char)(gimg_png_read_be16(src) >> 8);
          dst[2] = (unsigned char)(gimg_png_read_be16(src + 2) & 0xFFu);
          dst[3] = (unsigned char)(gimg_png_read_be16(src + 2) >> 8);
          dst[4] = (unsigned char)(gimg_png_read_be16(src + 4) & 0xFFu);
          dst[5] = (unsigned char)(gimg_png_read_be16(src + 4) >> 8);
          dst[6] = (unsigned char)(gimg_png_read_be16(src + 6) & 0xFFu);
          dst[7] = (unsigned char)(gimg_png_read_be16(src + 6) >> 8);
          src += 8;
          dst += 8;
        }
      }
    }
  }

  gimg_free(alloc, raw_full);

  GIMG_Result r = gimg_raster_create_with_allocator(alloc, ihdr->width,
      ihdr->height, format, GIMG_RASTER_OWNED, pixels, stride, out_raster);
  if (r != GIMG_OK) {
    gimg_free(alloc, pixels);
    return r;
  }

  // Apply color chunks (sRGB > iCCP > gAMA/cHRM); store in raster color_info.
  {
    GIMG_Color_Info color_info;
    void * icc_owned = NULL;
    size_t icc_size = 0;
    if (gimg_png_fill_color_info_from_ancillary(state, alloc, &color_info,
            &icc_owned, &icc_size)) {
      r = gimg_raster_set_color_info(*out_raster, &color_info);
      if (icc_owned) {
        gimg_free(alloc, icc_owned);
      }
      if (r != GIMG_OK) {
        gimg_raster_destroy(*out_raster);
        *out_raster = NULL;
        return r;
      }
    }
  }

  return GIMG_OK;
}

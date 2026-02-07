/**
 * @file
 *
 * PNG common: Adam7 interlace and row-bytes used by both decode and save.
 * Single place for pass table and dimensions, and for row-byte calculation
 * (all color types 0, 2, 3, 4, 6) so behavior is identical.
 *
 * References: W3C PNG DataRep §2.6 Interlaced data order (Adam7);
 * PNG §3.2 Image layout (sample depth, row bytes).
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <ghoti.io/compress/compress.h>
#include <ghoti.io/compress/options.h>
#include <ghoti.io/image/core.h>

#include "../../core/alloc_internal.h"
#include "../../core/safe_math_internal.h"
#include "png_internal.h"

/** Max decoded text size for zTXt/iTXt (bomb protection). */
#define GIMG_PNG_TEXT_MAX_DECODED (1024u * 1024u)

//
// Adam7 interlace (W3C PNG-DataRep §2.6). Seven passes with fixed x/y offset
// and step; pass_dims computes pass width/height as ceil((image - offset) /
// step) so decode and save use identical dimensions and row-byte counts.
//
const gimg_png_adam7_pass_t gimg_png_adam7_passes[7] = {
    {0, 0, 8, 8},
    {4, 0, 8, 8},
    {0, 4, 4, 8},
    {2, 0, 4, 4},
    {0, 2, 2, 4},
    {1, 0, 2, 2},
    {0, 1, 1, 2},
};

void gimg_png_adam7_pass_dims(uint32_t image_width, uint32_t image_height,
    unsigned int pass_index, uint32_t * out_pass_width,
    uint32_t * out_pass_height) {
  const gimg_png_adam7_pass_t * p = &gimg_png_adam7_passes[pass_index];
  *out_pass_width = (image_width > p->x_offset)
      ? (uint32_t)((image_width - p->x_offset + p->x_step - 1) / p->x_step)
      : 0;
  *out_pass_height = (image_height > p->y_offset)
      ? (uint32_t)((image_height - p->y_offset + p->y_step - 1) / p->y_step)
      : 0;
}

//
// Row bytes (PNG §3.2): samples per row from color_type and width, then
// bits per sample; result is (samples * bit_depth + 7) / 8. Used by both
// decode (buffer sizing, raw layout) and save (IDAT row layout).
//
size_t gimg_png_row_bytes(
    uint8_t color_type, uint8_t bit_depth, uint32_t width) {
  size_t samples_per_row = 0;
  switch (color_type) {
  case 0:
    samples_per_row = (size_t)width;
    break;
  case 2:
    samples_per_row = (size_t)width * 3u;
    break;
  case 3:
    samples_per_row = (size_t)width;
    break;
  case 4:
    samples_per_row = (size_t)width * 2u;
    break;
  case 6:
    samples_per_row = (size_t)width * 4u;
    break;
  default:
    return 0;
  }
  return (samples_per_row * (size_t)bit_depth + 7u) / 8u;
}

size_t gimg_png_row_bytes_from_ihdr(
    const gimg_png_ihdr_t * ihdr, uint32_t width) {
  if (!ihdr) {
    return 0;
  }
  return gimg_png_row_bytes(ihdr->color_type, ihdr->bit_depth, width);
}

//
// DEFLATE decode options: single helper so decode paths (single-frame and
// APNG) use the same limit handling (max_output_bytes from raw size). Avoids
// repeated create/set/destroy and ensures GCOMP_ERR_LIMIT is mapped
// consistently to GIMG_ERR_LIMIT.
//
GIMG_Result gimg_png_deflate_options_for_decode(
    size_t max_output_bytes, gcomp_options_t ** out_opts) {
  if (!out_opts) {
    return GIMG_ERR_INTERNAL;
  }
  *out_opts = NULL;
  gcomp_options_t * opts = NULL;
  gcomp_status_t gs = gcomp_options_create(&opts);
  if (gs != GCOMP_OK || !opts) {
    return (gs == GCOMP_ERR_MEMORY) ? GIMG_ERR_OOM : GIMG_ERR_INTERNAL;
  }
  gs = gcomp_options_set_uint64(
      opts, "limits.max_output_bytes", (uint64_t)max_output_bytes);
  if (gs != GCOMP_OK) {
    gcomp_options_destroy(opts);
    return GIMG_ERR_INTERNAL;
  }
  *out_opts = opts;
  return GIMG_OK;
}

bool gimg_png_adam7_raw_size(uint32_t width, uint32_t height, uint8_t color_type,
    uint8_t bit_depth, size_t * out_size) {
  size_t total = 0;
  for (int pass = 0; pass < 7; pass++) {
    uint32_t pw = 0;
    uint32_t ph = 0;
    gimg_png_adam7_pass_dims(width, height, (unsigned int)pass, &pw, &ph);
    if (pw == 0 || ph == 0) {
      continue;
    }
    size_t row_bytes = gimg_png_row_bytes(color_type, bit_depth, pw);
    size_t pass_row_stride = 1u + row_bytes;
    size_t pass_size = 0;
    if (!gimg_safe_mul_size((size_t)ph, pass_row_stride, &pass_size) ||
        !gimg_safe_add_size(total, pass_size, &total)) {
      return false;
    }
  }
  *out_size = total;
  return true;
}

//
// Text chunk decode (tEXt/zTXt/iTXt) for meta_common description.
//
GIMG_Result gimg_png_text_chunk_decode(gimg_png_chunk_type_t type,
    const unsigned char * payload, size_t payload_size,
    const GIMG_Allocator * alloc, size_t * out_keyword_len, char ** out_text) {
  if (!payload || payload_size == 0 || !alloc || !out_keyword_len ||
      !out_text) {
    return GIMG_ERR_INTERNAL;
  }
  *out_keyword_len = 0;
  *out_text = NULL;
  size_t kw_len = 0;
  while (kw_len < payload_size && payload[kw_len] != 0) {
    kw_len++;
  }
  if (kw_len >= payload_size) {
    return GIMG_ERR_FORMAT;
  }
  *out_keyword_len = kw_len;

  if (type == GIMG_PNG_tEXt) {
    size_t text_len = payload_size - kw_len - 1u;
    if (text_len > GIMG_PNG_TEXT_MAX_DECODED) {
      return GIMG_ERR_LIMIT;
    }
    char * text = (char *)gimg_malloc(alloc, text_len + 1u);
    if (!text) {
      return GIMG_ERR_OOM;
    }
    memcpy(text, payload + kw_len + 1u, text_len);
    text[text_len] = '\0';
    *out_text = text;
    return GIMG_OK;
  }

  if (type == GIMG_PNG_zTXt) {
    if (payload_size < kw_len + 3u) {
      return GIMG_ERR_FORMAT;
    }
    uint8_t comp = payload[kw_len + 1u];
    if (comp != 0) {
      return GIMG_ERR_UNSUPPORTED;
    }
    const unsigned char * zlib_src = payload + kw_len + 2u;
    size_t zlib_len = payload_size - kw_len - 2u;
    if (zlib_len <= 6u) {
      return GIMG_ERR_FORMAT;
    }
    const unsigned char * deflate_src = zlib_src + 2;
    size_t deflate_len = zlib_len - 6u;
    size_t max_out = GIMG_PNG_TEXT_MAX_DECODED;
    void * decoded = gimg_malloc(alloc, max_out);
    if (!decoded) {
      return GIMG_ERR_OOM;
    }
    size_t out_len = 0;
    gcomp_options_t * gopts = NULL;
    GIMG_Result r = gimg_png_deflate_options_for_decode(max_out, &gopts);
    if (r != GIMG_OK) {
      gimg_free(alloc, decoded);
      return r;
    }
    gcomp_status_t gs = gcomp_decode_buffer(gcomp_registry_default(), "deflate",
        gopts, deflate_src, deflate_len, decoded, max_out, &out_len);
    gcomp_options_destroy(gopts);
    if (gs != GCOMP_OK) {
      gimg_free(alloc, decoded);
      return (gs == GCOMP_ERR_MEMORY) ? GIMG_ERR_OOM
          : (gs == GCOMP_ERR_LIMIT) ? GIMG_ERR_LIMIT
          : GIMG_ERR_CORRUPT;
    }
    char * text = (char *)gimg_realloc(alloc, decoded, out_len + 1u);
    if (!text) {
      gimg_free(alloc, decoded);
      return GIMG_ERR_OOM;
    }
    text[out_len] = '\0';
    *out_text = text;
    return GIMG_OK;
  }

  if (type == GIMG_PNG_iTXt) {
    if (payload_size < kw_len + 5u) {
      return GIMG_ERR_FORMAT;
    }
    uint8_t comp_flag = payload[kw_len + 1u];
    uint8_t comp_method = payload[kw_len + 2u];
    size_t pos = kw_len + 3u;
    while (pos < payload_size && payload[pos] != 0) {
      pos++;
    }
    if (pos >= payload_size) {
      return GIMG_ERR_FORMAT;
    }
    pos++;
    while (pos < payload_size && payload[pos] != 0) {
      pos++;
    }
    if (pos >= payload_size) {
      return GIMG_ERR_FORMAT;
    }
    pos++;
    size_t text_src_len = payload_size - pos;
    if (comp_flag == 0) {
      if (text_src_len > GIMG_PNG_TEXT_MAX_DECODED) {
        return GIMG_ERR_LIMIT;
      }
      char * text = (char *)gimg_malloc(alloc, text_src_len + 1u);
      if (!text) {
        return GIMG_ERR_OOM;
      }
      memcpy(text, payload + pos, text_src_len);
      text[text_src_len] = '\0';
      *out_text = text;
      return GIMG_OK;
    }
    if (comp_method != 0 || text_src_len <= 6u) {
      return GIMG_ERR_FORMAT;
    }
    const unsigned char * deflate_src = payload + pos + 2;
    size_t deflate_len = text_src_len - 6u;
    size_t max_out = GIMG_PNG_TEXT_MAX_DECODED;
    void * decoded = gimg_malloc(alloc, max_out);
    if (!decoded) {
      return GIMG_ERR_OOM;
    }
    size_t out_len = 0;
    gcomp_options_t * gopts = NULL;
    GIMG_Result r = gimg_png_deflate_options_for_decode(max_out, &gopts);
    if (r != GIMG_OK) {
      gimg_free(alloc, decoded);
      return r;
    }
    gcomp_status_t gs = gcomp_decode_buffer(gcomp_registry_default(), "deflate",
        gopts, deflate_src, deflate_len, decoded, max_out, &out_len);
    gcomp_options_destroy(gopts);
    if (gs != GCOMP_OK) {
      gimg_free(alloc, decoded);
      return (gs == GCOMP_ERR_MEMORY) ? GIMG_ERR_OOM
          : (gs == GCOMP_ERR_LIMIT) ? GIMG_ERR_LIMIT
          : GIMG_ERR_CORRUPT;
    }
    char * text = (char *)gimg_realloc(alloc, decoded, out_len + 1u);
    if (!text) {
      gimg_free(alloc, decoded);
      return GIMG_ERR_OOM;
    }
    text[out_len] = '\0';
    *out_text = text;
    return GIMG_OK;
  }

  return GIMG_ERR_INTERNAL;
}

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
#include <ghoti.io/image/macros.h>
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
// zlib decode options: single helper so decode paths (single-frame and APNG)
// use the same limit handling (max_output_bytes from raw size). Avoids
// repeated create/set/destroy and ensures GCOMP_ERR_LIMIT is mapped
// consistently to GIMG_ERR_LIMIT.
//
GIMG_Result gimg_png_zlib_options_for_decode(
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

  // Turn the expansion-ratio limit off, deliberately.
  //
  // It is a sensible default for a caller decompressing something of unknown
  // size, and the zlib method sets it to 1000 for that reason.  Here the size
  // is not unknown: max_output_bytes above is the exact number of bytes IHDR
  // says the filtered rows come to, so the output is already bounded by the
  // thing the ratio is a proxy for.
  //
  // Leaving both on would reject legitimate images.  A 4096x4096 image of one
  // colour is 67,112,960 bytes of filtered rows and 65,141 bytes of zlib -
  // 1030:1, over the default, and a flat image only gets flatter as it gets
  // bigger.  The raw deflate path this replaced had no ratio limit at all, so
  // this keeps the behaviour those files already relied on.
  gs = gcomp_options_set_uint64(opts, "limits.max_expansion_ratio", 0);
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
    if (!gcu_safe_mul_size((size_t)ph, pass_row_stride, &pass_size) ||
        !gcu_safe_add_size(total, pass_size, &total)) {
      return false;
    }
  }
  *out_size = total;
  return true;
}

//
// zlib wrapper handling (PNG 10.3, RFC 1950).
//
// PNG stores IDAT, fdAT, zTXt, iTXt and iCCP payloads as a zlib stream: two
// header bytes, the DEFLATE data, and a four-byte Adler-32 of the
// *uncompressed* bytes.  Both of PNG's integrity checks matter - the chunk CRC
// catches damage to the stored bytes, the Adler-32 catches a stream that
// inflates without complaint but to the wrong thing - so neither the header
// nor the trailer is taken on trust here.
//
// This used to parse CMF and FLG by hand and carry its own Adler-32, because
// the compress library had raw deflate and gzip and nothing in between.  It
// has a zlib method now, so the container is its job and what is left here is
// only what PNG adds on top of RFC 1950.
//

GIMG_Result gimg_png_zlib_decode(const unsigned char * zlib_data,
    size_t zlib_size, unsigned char * out, size_t out_capacity,
    size_t * out_len) {
  if (!zlib_data || !out_len) {
    return GIMG_ERR_INTERNAL;
  }
  *out_len = 0;
  // Two header bytes plus a four-byte Adler-32; the DEFLATE data itself may be
  // empty, so anything shorter than six bytes is not a zlib stream at all.
  if (zlib_size < GIMG_PNG_ZLIB_MIN_BYTES) {
    return GIMG_ERR_FORMAT;
  }

  // The header is inspected before decoding, for two reasons.
  //
  // One is that PNG and RFC 1950 disagree about what is allowed, and PNG is
  // the stricter: 10.3 permits only compression method 8 and forbids a preset
  // dictionary outright.  A stream with FDICT set is a valid zlib stream that
  // is not a valid PNG payload, and it is this codec's job to say so.
  //
  // The other is that it keeps the distinction the caller sees.  A file that
  // is not a zlib stream at all is a format error; one that is, but whose
  // bytes have been altered, is corruption.  Handing everything to the
  // decoder would report both the same way.
  gcomp_zlib_header_info_t zinfo;
  if (gcomp_zlib_peek_header(zlib_data, zlib_size, &zinfo) != GCOMP_OK) {
    return GIMG_ERR_FORMAT;
  }
  if (zinfo.has_dictionary) {
    return GIMG_ERR_FORMAT;
  }

  gcomp_options_t * opts = NULL;
  GIMG_Result r = gimg_png_zlib_options_for_decode(out_capacity, &opts);
  if (r != GIMG_OK) {
    return r;
  }
  size_t produced = 0;
  gcomp_status_t gs = gcomp_decode_buffer(gcomp_registry_default(), "zlib",
      opts, zlib_data, zlib_size, out, out_capacity, &produced);
  gcomp_options_destroy(opts);
  if (gs != GCOMP_OK) {
    // Everything the zlib method rejects from here on is damage to a stream
    // that was well formed at the header: a truncated or malformed DEFLATE
    // body, or an Adler-32 over bytes other than the ones that came out.
    return (gs == GCOMP_ERR_MEMORY) ? GIMG_ERR_OOM
        : (gs == GCOMP_ERR_LIMIT)   ? GIMG_ERR_LIMIT
                                    : GIMG_ERR_CORRUPT;
  }

  *out_len = produced;
  return GIMG_OK;
}

//
// Sub-byte sample access (PNG 7.2).
//
// Depths 1, 2 and 4 pack several samples into a byte, most significant bits
// first: for depth 1 pixel 0 is bit 7, for depth 2 it is bits 7-6, for depth 4
// it is bits 7-4. A scanline is padded to a whole byte. Anything that moves
// samples between scanlines - the Adam7 reassembly on decode, the row packing
// on save - has to address them by bit, because at these depths a pixel index
// is not a byte offset.
//

uint8_t gimg_png_get_sample_bits(
    const unsigned char * row, uint32_t x, uint8_t depth) {
  unsigned int per_byte = 8u / (unsigned int)depth;
  size_t byte_index = (size_t)(x / per_byte);
  unsigned int within = (unsigned int)(x % per_byte);
  unsigned int shift = 8u - (unsigned int)depth * (within + 1u);
  return (uint8_t)((row[byte_index] >> shift) & ((1u << depth) - 1u));
}

void gimg_png_set_sample_bits(
    unsigned char * row, uint32_t x, uint8_t depth, uint8_t value) {
  unsigned int per_byte = 8u / (unsigned int)depth;
  size_t byte_index = (size_t)(x / per_byte);
  unsigned int within = (unsigned int)(x % per_byte);
  unsigned int shift = 8u - (unsigned int)depth * (within + 1u);
  unsigned int mask = ((1u << depth) - 1u) << shift;
  unsigned int bits = ((unsigned int)value << shift) & mask;
  row[byte_index] = (unsigned char)((row[byte_index] & ~mask) | bits);
}

//
// Physical pixel dimensions (PNG 11.3.4.3)
//
// pHYs states pixels per meter; the common metadata carries dots per inch,
// which is what JFIF and Exif state and so what the JPEG codec already reads
// and writes. An inch is exactly 0.0254 m, so both directions are integer
// arithmetic with explicit rounding rather than a float round trip - 5000/127
// and 127/5000. The intermediate is 64-bit because dpi * 5000 leaves the
// 32-bit range at about 859,000 dpi, which no sane file states but a hostile
// one may.
//

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
      // 11.3.3 defines compression method 0 and nothing else, so this is a
      // malformed chunk rather than a feature this library has not got round
      // to - which is what the iTXt path below already said about the same
      // byte. The caller skips a text chunk it cannot decode, so either way
      // the image still loads; the distinction is about what is true.
      return GIMG_ERR_FORMAT;
    }
    const unsigned char * zlib_src = payload + kw_len + 2u;
    size_t zlib_len = payload_size - kw_len - 2u;
    if (zlib_len <= 6u) {
      return GIMG_ERR_FORMAT;
    }
    size_t max_out = GIMG_PNG_TEXT_MAX_DECODED;
    void * decoded = gimg_malloc(alloc, max_out);
    if (!decoded) {
      return GIMG_ERR_OOM;
    }
    size_t out_len = 0;
    GIMG_Result r = gimg_png_zlib_decode(
        zlib_src, zlib_len, (unsigned char *)decoded, max_out, &out_len);
    if (r != GIMG_OK) {
      gimg_free(alloc, decoded);
      return r;
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
    size_t max_out = GIMG_PNG_TEXT_MAX_DECODED;
    void * decoded = gimg_malloc(alloc, max_out);
    if (!decoded) {
      return GIMG_ERR_OOM;
    }
    size_t out_len = 0;
    GIMG_Result r = gimg_png_zlib_decode(payload + pos, text_src_len,
        (unsigned char *)decoded, max_out, &out_len);
    if (r != GIMG_OK) {
      gimg_free(alloc, decoded);
      return r;
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

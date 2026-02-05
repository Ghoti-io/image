/**
 * @file
 *
 * PNG decode: DEFLATE decompress IDAT, apply filters, produce raster.
 *
 * Specification references:
 * - W3C PNG: https://www.w3.org/TR/PNG/ (Recommendation 10 Nov 2003)
 * - W3C Data representation (filtering, interlace): https://www.w3.org/TR/PNG-DataRep.html
 * - W3C Filter algorithms: https://www.w3.org/TR/PNG-Filters.html
 * - ISO/IEC 15948:2004 (PNG — Portable Network Graphics)
 * - Interlaced data order (Adam7): W3C §2.6
 *   https://www.w3.org/TR/PNG-DataRep.html#DR.Interlaced-data-order
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <ghoti.io/image/codec.h>
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

/** Bytes per row for a given pixel width (excluding filter byte). PNG §3.2. */
static size_t gimg_png_row_bytes_for_width(const gimg_png_ihdr_t * ihdr,
    uint32_t width) {
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
    {0, 0, 8, 8}, {4, 0, 8, 8}, {0, 4, 4, 8}, {2, 0, 4, 4},
    {0, 2, 2, 4}, {1, 0, 2, 2}, {0, 1, 1, 2},
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

/** Expected decompressed size for Adam7: sum over passes of rows × (1 + row_bytes). */
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
  case 0: /* None */
    (void)prior;
    (void)bpp;
    break;
  case 1: /* Sub */
  {
    (void)prior;
    for (size_t i = 0; i < n; i++) {
      unsigned char left = (i >= (size_t)bpp) ? raw[i - bpp] : 0;
      raw[i] = (unsigned char)((unsigned int)raw[i] + (unsigned int)left);
    }
    break;
  }
  case 2: /* Up */
  {
    for (size_t i = 0; i < n; i++) {
      unsigned char up = prior ? prior[i] : 0;
      raw[i] = (unsigned char)((unsigned int)raw[i] + (unsigned int)up);
    }
    break;
  }
  case 3: /* Average */
  {
    for (size_t i = 0; i < n; i++) {
      unsigned char left = (i >= (size_t)bpp) ? raw[i - bpp] : 0;
      unsigned char up = prior ? prior[i] : 0;
      raw[i] = (unsigned char)((unsigned int)raw[i] +
          ((unsigned int)left + (unsigned int)up) / 2);
    }
    break;
  }
  case 4: /* Paeth */
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
  if (!doc || doc->loaded_by_codec != codec || !doc->codec_private) {
    return GIMG_ERR_UNSUPPORTED;
  }

  gimg_png_doc_state_t * state = (gimg_png_doc_state_t *)doc->codec_private;
  if (item->index >= 1) {
    return GIMG_ERR_UNSUPPORTED; /* Single-frame only for now. */
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

  unsigned char * raw = (unsigned char *)gimg_malloc(alloc, raw_size);
  if (!raw) {
    gcomp_options_destroy(gopts);
    return GIMG_ERR_OOM;
  }

  size_t out_len = 0;
  gs = gcomp_decode_buffer(gcomp_registry_default(), "deflate", gopts,
      state->idat, state->idat_size, raw, raw_size, &out_len);
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

  /* Build unfiltered image in row-major form (no filter bytes) for raster copy. */
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
    /* Adam7: each pass is filtered independently; scatter into raw_full. */
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

  /* Build raster in a canonical format (1.2.1: grayscale/RGB 8-bit). */
  const GIMG_Pixel_Format * format = NULL;
  switch (ihdr->color_type) {
  case 0:
    format = &GIMG_PIXEL_GRAY8;
    break;
  case 2:
    format = &GIMG_PIXEL_RGBA8;
    break;
  case 3:
    format = &GIMG_PIXEL_RGBA8; /* Palette -> expand in 1.2.2; use RGBA8. */
    break;
  case 4:
    format = &GIMG_PIXEL_RGBA8; /* Gray+alpha -> RGBA8 (R=G=B=gray, A=alpha). */
    break;
  case 6:
    format = &GIMG_PIXEL_RGBA8;
    break;
  default:
    gimg_free(alloc, raw);
    return GIMG_ERR_FORMAT;
  }

  if (ihdr->bit_depth != 8) {
    gimg_free(alloc, raw);
    return GIMG_ERR_UNSUPPORTED; /* 16-bit in 1.2.2. */
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

  /* Copy unfiltered raw_full (row stride row_bytes) into output raster. */
  if (ihdr->color_type == 0) {
    for (uint32_t y = 0; y < h; y++) {
      const unsigned char * src = raw_full + (size_t)y * row_bytes;
      unsigned char * dst = (unsigned char *)pixels + (size_t)y * stride;
      memcpy(dst, src, row_bytes);
    }
  }
  else if (ihdr->color_type == 4) {
    for (uint32_t y = 0; y < h; y++) {
      const unsigned char * src = raw_full + (size_t)y * row_bytes;
      unsigned char * dst = (unsigned char *)pixels + (size_t)y * stride;
      for (size_t x = 0; x < (size_t)w; x++) {
        unsigned char g = src[0];
        unsigned char a = src[1];
        dst[0] = g;
        dst[1] = g;
        dst[2] = g;
        dst[3] = a;
        src += 2;
        dst += 4;
      }
    }
  }
  else if (ihdr->color_type == 2) {
    for (uint32_t y = 0; y < h; y++) {
      const unsigned char * src = raw_full + (size_t)y * row_bytes;
      unsigned char * dst = (unsigned char *)pixels + (size_t)y * stride;
      for (size_t x = 0; x < (size_t)w; x++) {
        dst[0] = src[0];
        dst[1] = src[1];
        dst[2] = src[2];
        dst[3] = 0xFF;
        src += 3;
        dst += 4;
      }
    }
  }
  else if (ihdr->color_type == 6) {
    for (uint32_t y = 0; y < h; y++) {
      const unsigned char * src = raw_full + (size_t)y * row_bytes;
      unsigned char * dst = (unsigned char *)pixels + (size_t)y * stride;
      memcpy(dst, src, (size_t)w * 4);
    }
  }
  else if (ihdr->color_type == 3) {
    if (!state->plte || state->plte_size < 3) {
      gimg_free(alloc, pixels);
      gimg_free(alloc, raw_full);
      return GIMG_ERR_FORMAT;
    }
    for (uint32_t y = 0; y < h; y++) {
      const unsigned char * src = raw_full + (size_t)y * row_bytes;
      unsigned char * dst = (unsigned char *)pixels + (size_t)y * stride;
      for (size_t x = 0; x < (size_t)w; x++) {
        unsigned int i = (unsigned int)(*src++) * 3;
        dst[0] = i + 0 < state->plte_size ? state->plte[i + 0] : 0;
        dst[1] = i + 1 < state->plte_size ? state->plte[i + 1] : 0;
        dst[2] = i + 2 < state->plte_size ? state->plte[i + 2] : 0;
        dst[3] = 0xFF;
        dst += 4;
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
  return GIMG_OK;
}

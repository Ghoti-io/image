/**
 * @file
 *
 * PNG decode one frame: DEFLATE decompress IDAT/fdAT, apply row filters,
 * Adam7 reassembly, raw-to-pixels. Used by single-frame and APNG decode paths.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "../../core/safe_math_internal.h"
#include <ghoti.io/image/codec.h>
#include <ghoti.io/image/raster.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <ghoti.io/compress/compress.h>
#include <ghoti.io/compress/options.h>

#include "../../core/alloc_internal.h"
#include "../../raster/raster_internal.h"
#include "png_internal.h"

/** Map compress status to image result (decode path). */
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

/** Bytes per pixel for raw PNG (filter byte excluded). */
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

GIMG_Result gimg_png_decode_idat_to_pixels(const gimg_png_doc_state_t * state,
    const gimg_png_ihdr_t * ihdr, const unsigned char * idat_ptr,
    size_t idat_len, uint32_t w, uint32_t h, const GIMG_Pixel_Format * format,
    const GIMG_Allocator * alloc, const GIMG_Limits * limits,
    void ** out_pixels, size_t * out_stride) {
  if (!out_pixels || !out_stride) {
    return GIMG_ERR_INTERNAL;
  }
  *out_pixels = NULL;
  *out_stride = 0;

  if (!idat_ptr || idat_len < GIMG_PNG_ZLIB_MIN_BYTES) {
    return GIMG_ERR_CORRUPT; /* Truncated or empty zlib payload. */
  }
  size_t row_bytes = gimg_png_row_bytes_from_ihdr(ihdr, w);
  size_t raw_size = 0;
  if (ihdr->interlace_method == 0) {
    if (row_bytes == 0) {
      return GIMG_ERR_FORMAT;
    }
    size_t row_stride = 1 + row_bytes;
    if (!gimg_safe_mul_size((size_t)h, row_stride, &raw_size)) {
      return GIMG_ERR_LIMIT;
    }
  }
  else {
    size_t total = 0;
    for (int pass = 0; pass < 7; pass++) {
      uint32_t pw = 0, ph = 0;
      gimg_png_adam7_pass_dims(w, h, (unsigned int)pass, &pw, &ph);
      if (pw == 0 || ph == 0) {
        continue;
      }
      size_t pass_row_bytes = gimg_png_row_bytes_from_ihdr(ihdr, pw);
      size_t pass_row_stride = 1 + pass_row_bytes;
      size_t pass_size = 0;
      if (!gimg_safe_mul_size((size_t)ph, pass_row_stride, &pass_size) ||
          !gimg_safe_add_size(total, pass_size, &total)) {
        return GIMG_ERR_LIMIT;
      }
    }
    raw_size = total;
  }
  size_t pixel_count = 0;
  if (gimg_safe_pixel_count(w, h, &pixel_count) != GIMG_OK) {
    return GIMG_ERR_LIMIT;
  }
  if (limits && limits->max_decoded_pixels != 0 &&
      pixel_count > limits->max_decoded_pixels) {
    return GIMG_ERR_LIMIT;
  }
  gcomp_options_t * gopts = NULL;
  GIMG_Result gr = gimg_png_deflate_options_for_decode(raw_size, &gopts);
  if (gr != GIMG_OK) {
    return gr;
  }
  unsigned char * raw = (unsigned char *)gimg_malloc(alloc, raw_size);
  if (!raw) {
    gcomp_options_destroy(gopts);
    return GIMG_ERR_OOM;
  }
  size_t out_len = 0;
  gcomp_status_t gs = gcomp_decode_buffer(gcomp_registry_default(), "deflate",
      gopts, idat_ptr + 2, idat_len - GIMG_PNG_ZLIB_MIN_BYTES, raw, raw_size,
      &out_len);
  gcomp_options_destroy(gopts);
  if (gs != GCOMP_OK || out_len != raw_size) {
    gimg_free(alloc, raw);
    return (gs != GCOMP_OK) ? gimg_png_result_from_gcomp(gs) : GIMG_ERR_CORRUPT;
  }
  unsigned int bpp = gimg_png_bpp(ihdr);
  size_t raw_full_size = 0;
  if (!gimg_safe_mul_size((size_t)h, row_bytes, &raw_full_size)) {
    gimg_free(alloc, raw);
    return GIMG_ERR_LIMIT;
  }
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
    size_t raw_off = 0;
    for (int pass = 0; pass < 7; pass++) {
      uint32_t pw = 0, ph = 0;
      gimg_png_adam7_pass_dims(w, h, (unsigned int)pass, &pw, &ph);
      if (pw == 0 || ph == 0) {
        continue;
      }
      const gimg_png_adam7_pass_t * ap = &gimg_png_adam7_passes[pass];
      size_t pass_row_bytes = gimg_png_row_bytes_from_ihdr(ihdr, pw);
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
  size_t bpp_out = gimg_raster_bytes_per_pixel(format);
  size_t stride = (size_t)w * bpp_out;
  if (stride % GIMG_DEFAULT_STRIDE_ALIGNMENT) {
    stride = (stride + GIMG_DEFAULT_STRIDE_ALIGNMENT - 1) &
        ~(size_t)(GIMG_DEFAULT_STRIDE_ALIGNMENT - 1);
  }
  void * pixels = gimg_malloc(alloc, stride * (size_t)h);
  if (!pixels) {
    gimg_free(alloc, raw_full);
    return GIMG_ERR_OOM;
  }
  memset(pixels, 0, stride * (size_t)h);
  gimg_png_raw_full_to_pixels(
      state, ihdr, format, raw_full, w, h, row_bytes, pixels, stride);
  gimg_free(alloc, raw_full);
  *out_pixels = pixels;
  *out_stride = stride;
  return GIMG_OK;
}

GIMG_Result gimg_png_decode_one_apng_frame(const gimg_png_doc_state_t * state,
    const gimg_png_ihdr_t * ihdr, size_t frame_index,
    const GIMG_Pixel_Format * format, const GIMG_Allocator * alloc,
    const GIMG_Limits * limits, void ** out_pixels, size_t * out_stride,
    uint32_t * out_fw, uint32_t * out_fh) {
  if (!state->frames || frame_index >= state->frame_count) {
    return GIMG_ERR_FORMAT;
  }
  const gimg_png_frame_t * frame = &state->frames[frame_index];
  uint32_t fw = frame->fctl.width;
  uint32_t fh = frame->fctl.height;
  GIMG_Result r = gimg_png_decode_idat_to_pixels(state, ihdr, frame->data,
      frame->data_size, fw, fh, format, alloc, limits, out_pixels, out_stride);
  if (r == GIMG_OK && out_fw) {
    *out_fw = fw;
  }
  if (r == GIMG_OK && out_fh) {
    *out_fh = fh;
  }
  return r;
}

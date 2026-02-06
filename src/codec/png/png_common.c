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

#include <ghoti.io/compress/compress.h>
#include <ghoti.io/compress/options.h>
#include <ghoti.io/image/core.h>

#include "../../core/safe_math_internal.h"
#include "png_internal.h"

//
// Adam7 (W3C §2.6)
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
// Row bytes (PNG §3.2: samples per row, bits per sample)
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
// DEFLATE decode options (shared by decode paths; limits handling consistent)
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

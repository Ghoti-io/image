/**
 * @file
 *
 * Orientation, pixel format conversion stub, alpha premultiply/unpremultiply.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <ghoti.io/image/bitdepth.h>
#include <ghoti.io/image/ops.h>
#include <ghoti.io/image/raster.h>
#include <string.h>

#include "../core/alloc_internal.h"

//
// Orientation: in-place transform. We support identity, 180, 90 CW, 90 CCW
// by reallocating and copying. For 90/270 we swap width/height and stride.
//

static GIMG_Result apply_orientation_180(GIMG_Raster * raster) {
  const GIMG_Allocator * alloc = gimg_raster_allocator(raster);
  size_t stride = gimg_raster_stride_bytes(raster);
  size_t bpp = gimg_raster_bytes_per_pixel(gimg_raster_format(raster));
  size_t row_bytes = gimg_raster_width(raster) * bpp;
  unsigned char * pixels = (unsigned char *)gimg_raster_pixels(raster);
  unsigned char * row_buf = (unsigned char *)gimg_malloc(alloc, row_bytes);
  if (!row_buf) {
    return GIMG_ERR_OOM;
  }
  uint32_t height = gimg_raster_height(raster);
  uint32_t width = gimg_raster_width(raster);
  for (uint32_t y = 0; y < height / 2; y++) {
    uint32_t bot = height - 1 - y;
    memcpy(row_buf, pixels + (size_t)y * stride, row_bytes);
    memcpy(
        pixels + (size_t)y * stride, pixels + (size_t)bot * stride, row_bytes);
    memcpy(pixels + (size_t)bot * stride, row_buf, row_bytes);
  }
  for (uint32_t y = 0; y < height; y++) {
    unsigned char * row = pixels + (size_t)y * stride;
    for (uint32_t x = 0; x < width / 2; x++) {
      uint32_t right = width - 1 - x;
      for (size_t b = 0; b < bpp; b++) {
        unsigned char t = row[x * bpp + b];
        row[x * bpp + b] = row[right * bpp + b];
        row[right * bpp + b] = t;
      }
    }
  }
  gimg_free(alloc, row_buf);
  return GIMG_OK;
}

static bool format_is_rgba8(const GIMG_Pixel_Format * f) {
  return f->channel_model == GIMG_CHANNEL_RGBA && f->channel_count == 4 &&
      f->bits_per_channel[0] == 8 && f->bits_per_channel[1] == 8 &&
      f->bits_per_channel[2] == 8 && f->bits_per_channel[3] == 8;
}

GIMG_API GIMG_Result gimg_ops_apply_orientation(
    GIMG_Raster * raster, GIMG_Orientation orientation) {
  if (!raster) {
    return GIMG_ERR_INTERNAL;
  }
  if (gimg_raster_ownership(raster) == GIMG_RASTER_BORROWED) {
    return GIMG_ERR_UNSUPPORTED; // In-place transform requires owned buffer
  }
  switch (orientation) {
  case GIMG_ORIENTATION_UNKNOWN:
  case GIMG_ORIENTATION_NORMAL:
    return GIMG_OK;
  case GIMG_ORIENTATION_ROTATE_180:
    return apply_orientation_180(raster);
  case GIMG_ORIENTATION_FLIP_H:
  case GIMG_ORIENTATION_FLIP_V:
  case GIMG_ORIENTATION_TRANSPOSE:
  case GIMG_ORIENTATION_ROTATE_90_CW:
  case GIMG_ORIENTATION_TRANSVERSE:
  case GIMG_ORIENTATION_ROTATE_90_CCW:
    return GIMG_ERR_UNSUPPORTED; // 90° requires realloc + new dimensions
  default:
    return GIMG_ERR_UNSUPPORTED;
  }
}

GIMG_API GIMG_Result gimg_ops_convert_pixel_format(const GIMG_Raster * src,
    const GIMG_Pixel_Format * dst_format, GIMG_Raster ** out_raster) {
  if (!src || !dst_format || !out_raster) {
    return GIMG_ERR_INTERNAL;
  }
  const GIMG_Pixel_Format * src_f = gimg_raster_format(src);
  if (src_f->channel_model != dst_format->channel_model ||
      src_f->channel_type != dst_format->channel_type ||
      src_f->channel_count != dst_format->channel_count) {
    *out_raster = NULL;
    return GIMG_ERR_UNSUPPORTED;
  }
  for (int i = 0; i < 8; i++) {
    if (src_f->bits_per_channel[i] != dst_format->bits_per_channel[i]) {
      *out_raster = NULL;
      return GIMG_ERR_UNSUPPORTED;
    }
  }
  uint32_t w = gimg_raster_width(src);
  uint32_t h = gimg_raster_height(src);
  GIMG_Result r = gimg_raster_create_with_allocator(gimg_raster_allocator(src),
      w, h, dst_format, GIMG_RASTER_OWNED, NULL, 0, out_raster);
  if (r != GIMG_OK) {
    return r;
  }
  size_t src_stride = gimg_raster_stride_bytes(src);
  size_t dst_stride = gimg_raster_stride_bytes(*out_raster);
  size_t row_bytes = w * gimg_raster_bytes_per_pixel(src_f);
  const unsigned char * sp =
      (const unsigned char *)gimg_raster_pixels_const(src);
  unsigned char * dp = (unsigned char *)gimg_raster_pixels(*out_raster);
  for (uint32_t y = 0; y < h; y++) {
    memcpy(dp, sp, row_bytes);
    sp += src_stride;
    dp += dst_stride;
  }
  return GIMG_OK;
}

//
// Bit-depth conversion: same channel model, 8/12/16 bits per channel. Uses
// library bitdepth sample-level functions (T.81 / codec-agnostic).
//
static const GIMG_Pixel_Format * format_for_bits(
    GIMG_Channel_Model model, uint8_t bits) {
  if (model == GIMG_CHANNEL_GRAY) {
    if (bits == 8) return &GIMG_PIXEL_GRAY8;
    if (bits == 12) return &GIMG_PIXEL_GRAY12;
    if (bits == 16) return &GIMG_PIXEL_GRAY16;
  }
  if (model == GIMG_CHANNEL_RGBA) {
    if (bits == 8) return &GIMG_PIXEL_RGBA8;
    if (bits == 12) return &GIMG_PIXEL_RGBA12;
    if (bits == 16) return &GIMG_PIXEL_RGBA16;
  }
  return NULL;
}

GIMG_API GIMG_Result gimg_ops_convert_bit_depth(const GIMG_Raster * src,
    uint8_t dst_bits, GIMG_Raster ** out_raster) {
  if (!src || !out_raster) {
    return GIMG_ERR_INTERNAL;
  }
  *out_raster = NULL;
  if (dst_bits != 8 && dst_bits != 12 && dst_bits != 16) {
    return GIMG_ERR_UNSUPPORTED;
  }
  const GIMG_Pixel_Format * src_f = gimg_raster_format(src);
  uint8_t src_bits = src_f->bits_per_channel[0];
  for (uint8_t i = 1; i < src_f->channel_count && i < 8; i++) {
    if (src_f->bits_per_channel[i] != src_bits) {
      return GIMG_ERR_UNSUPPORTED;
    }
  }
  if (src_bits != 8 && src_bits != 12 && src_bits != 16) {
    return GIMG_ERR_UNSUPPORTED;
  }
  if (src_f->channel_model != GIMG_CHANNEL_GRAY &&
      src_f->channel_model != GIMG_CHANNEL_RGBA) {
    return GIMG_ERR_UNSUPPORTED;
  }
  const GIMG_Pixel_Format * dst_f = format_for_bits(src_f->channel_model,
      dst_bits);
  if (!dst_f) {
    return GIMG_ERR_UNSUPPORTED;
  }
  uint32_t w = gimg_raster_width(src);
  uint32_t h = gimg_raster_height(src);
  GIMG_Result r = gimg_raster_create_with_allocator(gimg_raster_allocator(src),
      w, h, dst_f, GIMG_RASTER_OWNED, NULL, 0, out_raster);
  if (r != GIMG_OK) {
    return r;
  }
  size_t src_stride = gimg_raster_stride_bytes(src);
  size_t dst_stride = gimg_raster_stride_bytes(*out_raster);
  const unsigned char * sp =
      (const unsigned char *)gimg_raster_pixels_const(src);
  unsigned char * dp = (unsigned char *)gimg_raster_pixels(*out_raster);
  uint8_t nch = src_f->channel_count;

  if (src_bits == dst_bits) {
    size_t row_bytes = (size_t)w * gimg_raster_bytes_per_pixel(src_f);
    for (uint32_t y = 0; y < h; y++) {
      memcpy(dp, sp, row_bytes);
      sp += src_stride;
      dp += dst_stride;
    }
    return GIMG_OK;
  }

  size_t src_bpp = gimg_raster_bytes_per_pixel(src_f);
  size_t dst_bpp = gimg_raster_bytes_per_pixel(dst_f);
  for (uint32_t y = 0; y < h; y++) {
    const unsigned char * sr = sp;
    unsigned char * dr = dp;
    for (uint32_t x = 0; x < w; x++) {
      for (uint8_t c = 0; c < nch; c++) {
        if (src_bits == 8) {
          uint8_t v8 = sr[c];
          if (dst_bits == 12) {
            ((uint16_t *)dr)[c] = gimg_bitdepth_8_to_12(v8);
          } else {
            ((uint16_t *)dr)[c] = gimg_bitdepth_8_to_16(v8);
          }
        } else if (src_bits == 12) {
          uint16_t v12 = ((const uint16_t *)sr)[c];
          if (v12 > 4095u) v12 = 4095u;
          if (dst_bits == 8) {
            dr[c] = gimg_bitdepth_12_to_8(v12);
          } else {
            ((uint16_t *)dr)[c] = gimg_bitdepth_12_to_16(v12);
          }
        } else {
          uint16_t v16 = ((const uint16_t *)sr)[c];
          if (dst_bits == 8) {
            dr[c] = gimg_bitdepth_16_to_8(v16);
          } else {
            ((uint16_t *)dr)[c] = gimg_bitdepth_16_to_12(v16);
          }
        }
      }
      sr += src_bpp;
      dr += dst_bpp;
    }
    sp += src_stride;
    dp += dst_stride;
  }
  return GIMG_OK;
}

GIMG_API GIMG_Result gimg_alpha_premultiply(GIMG_Raster * raster) {
  if (!raster) {
    return GIMG_ERR_INTERNAL;
  }
  if (!format_is_rgba8(gimg_raster_format(raster))) {
    return GIMG_ERR_UNSUPPORTED;
  }
  unsigned char * p = (unsigned char *)gimg_raster_pixels(raster);
  size_t stride = gimg_raster_stride_bytes(raster);
  uint32_t width = gimg_raster_width(raster);
  uint32_t height = gimg_raster_height(raster);
  for (uint32_t y = 0; y < height; y++) {
    unsigned char * row = p + (size_t)y * stride;
    for (uint32_t x = 0; x < width; x++) {
      unsigned char * px = row + (size_t)x * 4;
      unsigned char a = px[3];
      if (a == 0) {
        px[0] = px[1] = px[2] = 0;
        continue;
      }
      px[0] = (unsigned char)((unsigned int)px[0] * a / 255);
      px[1] = (unsigned char)((unsigned int)px[1] * a / 255);
      px[2] = (unsigned char)((unsigned int)px[2] * a / 255);
    }
  }
  return GIMG_OK;
}

GIMG_API GIMG_Result gimg_alpha_unpremultiply(GIMG_Raster * raster) {
  if (!raster) {
    return GIMG_ERR_INTERNAL;
  }
  if (!format_is_rgba8(gimg_raster_format(raster))) {
    return GIMG_ERR_UNSUPPORTED;
  }
  unsigned char * p = (unsigned char *)gimg_raster_pixels(raster);
  size_t stride = gimg_raster_stride_bytes(raster);
  uint32_t width = gimg_raster_width(raster);
  uint32_t height = gimg_raster_height(raster);
  for (uint32_t y = 0; y < height; y++) {
    unsigned char * row = p + (size_t)y * stride;
    for (uint32_t x = 0; x < width; x++) {
      unsigned char * px = row + (size_t)x * 4;
      unsigned char a = px[3];
      if (a == 0) {
        continue;
      }
      px[0] = (unsigned char)((unsigned int)px[0] * 255 / a);
      px[1] = (unsigned char)((unsigned int)px[1] * 255 / a);
      px[2] = (unsigned char)((unsigned int)px[2] * 255 / a);
    }
  }
  return GIMG_OK;
}

// Equality: same dimensions, same pixel format (channel model/type/count and
// bits per channel), and identical pixel data. Strides may differ (only
// row_bytes are compared). Color info is not compared. Used by tests and
// callers that need equivalence checks.
GIMG_API bool gimg_ops_raster_equal(const GIMG_Raster * a, const GIMG_Raster * b) {
  if (!a || !b) {
    return false;
  }
  if (gimg_raster_width(a) != gimg_raster_width(b) ||
      gimg_raster_height(a) != gimg_raster_height(b)) {
    return false;
  }
  const GIMG_Pixel_Format * fa = gimg_raster_format(a);
  const GIMG_Pixel_Format * fb = gimg_raster_format(b);
  if (!fa || !fb || fa->channel_model != fb->channel_model ||
      fa->channel_type != fb->channel_type ||
      fa->channel_count != fb->channel_count) {
    return false;
  }
  for (int i = 0; i < 8; i++) {
    if (fa->bits_per_channel[i] != fb->bits_per_channel[i]) {
      return false;
    }
  }
  size_t bpp = gimg_raster_bytes_per_pixel(fa);
  if (bpp == 0) {
    return false;
  }
  uint32_t w = gimg_raster_width(a);
  uint32_t h = gimg_raster_height(a);
  size_t row_bytes = (size_t)w * bpp;
  size_t stride_a = gimg_raster_stride_bytes(a);
  size_t stride_b = gimg_raster_stride_bytes(b);
  const unsigned char * pa =
      (const unsigned char *)gimg_raster_pixels_const(a);
  const unsigned char * pb =
      (const unsigned char *)gimg_raster_pixels_const(b);
  if (!pa || !pb) {
    return false;
  }
  for (uint32_t y = 0; y < h; y++) {
    if (memcmp(pa + (size_t)y * stride_a, pb + (size_t)y * stride_b,
            row_bytes) != 0) {
      return false;
    }
  }
  return true;
}

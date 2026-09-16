/**
 * @file
 *
 * Raster create/destroy and accessors.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <ghoti.io/image/macros.h>
#include <ghoti.io/image/color.h>
#include <ghoti.io/image/raster.h>
#include <string.h>

#include "../core/alloc_internal.h"
#include "raster_internal.h"

// Canonical format definitions.
static const GIMG_Pixel_Format gimg_pixel_rgba8 = {
    .channel_model = GIMG_CHANNEL_RGBA,
    .channel_type = GIMG_CHANNEL_UNORM,
    .layout = GIMG_LAYOUT_INTERLEAVED,
    .channel_count = 4,
    .bits_per_channel = {8, 8, 8, 8, 0, 0, 0, 0},
    .alignment = GIMG_DEFAULT_STRIDE_ALIGNMENT,
    ._reserved = {0},
};

static const GIMG_Pixel_Format gimg_pixel_rgba16 = {
    .channel_model = GIMG_CHANNEL_RGBA,
    .channel_type = GIMG_CHANNEL_UNORM,
    .layout = GIMG_LAYOUT_INTERLEAVED,
    .channel_count = 4,
    .bits_per_channel = {16, 16, 16, 16, 0, 0, 0, 0},
    .alignment = GIMG_DEFAULT_STRIDE_ALIGNMENT,
    ._reserved = {0},
};

static const GIMG_Pixel_Format gimg_pixel_gray8 = {
    .channel_model = GIMG_CHANNEL_GRAY,
    .channel_type = GIMG_CHANNEL_UNORM,
    .layout = GIMG_LAYOUT_INTERLEAVED,
    .channel_count = 1,
    .bits_per_channel = {8, 0, 0, 0, 0, 0, 0, 0},
    .alignment = GIMG_DEFAULT_STRIDE_ALIGNMENT,
    ._reserved = {0},
};

static const GIMG_Pixel_Format gimg_pixel_gray16 = {
    .channel_model = GIMG_CHANNEL_GRAY,
    .channel_type = GIMG_CHANNEL_UNORM,
    .layout = GIMG_LAYOUT_INTERLEAVED,
    .channel_count = 1,
    .bits_per_channel = {16, 0, 0, 0, 0, 0, 0, 0},
    .alignment = GIMG_DEFAULT_STRIDE_ALIGNMENT,
    ._reserved = {0},
};

static const GIMG_Pixel_Format gimg_pixel_gray12 = {
    .channel_model = GIMG_CHANNEL_GRAY,
    .channel_type = GIMG_CHANNEL_UNORM,
    .layout = GIMG_LAYOUT_INTERLEAVED,
    .channel_count = 1,
    .bits_per_channel = {12, 0, 0, 0, 0, 0, 0, 0},
    .alignment = GIMG_DEFAULT_STRIDE_ALIGNMENT,
    ._reserved = {0},
};

static const GIMG_Pixel_Format gimg_pixel_rgba12 = {
    .channel_model = GIMG_CHANNEL_RGBA,
    .channel_type = GIMG_CHANNEL_UNORM,
    .layout = GIMG_LAYOUT_INTERLEAVED,
    .channel_count = 4,
    .bits_per_channel = {12, 12, 12, 12, 0, 0, 0, 0},
    .alignment = GIMG_DEFAULT_STRIDE_ALIGNMENT,
    ._reserved = {0},
};

static const GIMG_Pixel_Format gimg_pixel_cmyk8 = {
    .channel_model = GIMG_CHANNEL_CMYK,
    .channel_type = GIMG_CHANNEL_UNORM,
    .layout = GIMG_LAYOUT_INTERLEAVED,
    .channel_count = 4,
    .bits_per_channel = {8, 8, 8, 8, 0, 0, 0, 0},
    .alignment = GIMG_DEFAULT_STRIDE_ALIGNMENT,
    ._reserved = {0},
};

// A twelve-bit four-component frame (T.81 Table B.2 allows P=12 in a DCT
// frame, and B.2.2 allows Nf=4) decodes to this, left-justified in 16 bits the
// way GRAY16 and RGBA16 carry their twelve-bit samples.  Without it such a
// frame had nowhere to go and was refused.
static const GIMG_Pixel_Format gimg_pixel_cmyk12 = {
    .channel_model = GIMG_CHANNEL_CMYK,
    .channel_type = GIMG_CHANNEL_UNORM,
    .layout = GIMG_LAYOUT_INTERLEAVED,
    .channel_count = 4,
    .bits_per_channel = {12, 12, 12, 12, 0, 0, 0, 0},
    .alignment = GIMG_DEFAULT_STRIDE_ALIGNMENT,
    ._reserved = {0},
};

static const GIMG_Pixel_Format gimg_pixel_cmyk16 = {
    .channel_model = GIMG_CHANNEL_CMYK,
    .channel_type = GIMG_CHANNEL_UNORM,
    .layout = GIMG_LAYOUT_INTERLEAVED,
    .channel_count = 4,
    .bits_per_channel = {16, 16, 16, 16, 0, 0, 0, 0},
    .alignment = GIMG_DEFAULT_STRIDE_ALIGNMENT,
    ._reserved = {0},
};

GIMG_API const GIMG_Pixel_Format GIMG_PIXEL_RGBA8 = gimg_pixel_rgba8;
GIMG_API const GIMG_Pixel_Format GIMG_PIXEL_RGBA16 = gimg_pixel_rgba16;
GIMG_API const GIMG_Pixel_Format GIMG_PIXEL_GRAY8 = gimg_pixel_gray8;
GIMG_API const GIMG_Pixel_Format GIMG_PIXEL_GRAY16 = gimg_pixel_gray16;
GIMG_API const GIMG_Pixel_Format GIMG_PIXEL_GRAY12 = gimg_pixel_gray12;
GIMG_API const GIMG_Pixel_Format GIMG_PIXEL_RGBA12 = gimg_pixel_rgba12;
GIMG_API const GIMG_Pixel_Format GIMG_PIXEL_CMYK8 = gimg_pixel_cmyk8;
GIMG_API const GIMG_Pixel_Format GIMG_PIXEL_CMYK12 = gimg_pixel_cmyk12;
GIMG_API const GIMG_Pixel_Format GIMG_PIXEL_CMYK16 = gimg_pixel_cmyk16;

GIMG_API uint8_t gimg_pixel_format_channel_bits(
    const GIMG_Pixel_Format * format, uint8_t index) {
  if (!format || index >= format->channel_count) {
    return 0;
  }
  // Past the eighth channel every channel has the depth of the first; see the
  // note on GIMG_Pixel_Format.bits_per_channel.
  return (index < 8u) ? format->bits_per_channel[index]
                      : format->bits_per_channel[0];
}

GIMG_API GIMG_Result gimg_pixel_format_multichannel(
    uint8_t channel_count, uint8_t bits, GIMG_Pixel_Format * out_format) {
  if (!out_format || channel_count == 0u ||
      (bits != 8u && bits != 12u && bits != 16u)) {
    return GIMG_ERR_UNSUPPORTED;
  }
  memset(out_format, 0, sizeof(*out_format));
  out_format->channel_model = (channel_count == 1u) ? GIMG_CHANNEL_GRAY
      : (channel_count == 4u)                       ? GIMG_CHANNEL_CMYK
                                                    : GIMG_CHANNEL_UNKNOWN;
  out_format->channel_type = GIMG_CHANNEL_UNORM;
  out_format->layout = GIMG_LAYOUT_INTERLEAVED;
  out_format->channel_count = channel_count;
  for (uint8_t i = 0; i < channel_count && i < 8u; i++) {
    out_format->bits_per_channel[i] = bits;
  }
  out_format->bits_per_channel[0] = bits;
  out_format->alignment = GIMG_DEFAULT_STRIDE_ALIGNMENT;
  return GIMG_OK;
}

GIMG_API size_t gimg_raster_bytes_per_pixel(const GIMG_Pixel_Format * format) {
  if (!format || format->channel_count == 0) {
    return 0;
  }
  // 12-bit is stored as uint16_t per sample (0..4095); 8- and 16-bit as usual.
  if (format->bits_per_channel[0] == 12) {
    return (size_t)format->channel_count * 2u;
  }
  // Past the eighth channel every channel has the depth of the first, so the
  // sum is that depth times the count rather than a walk off the end of the
  // array.  The loop used to stop at eight and return a pixel size for eight
  // channels however many there were.
  if (format->channel_count > 8u) {
    return ((size_t)format->channel_count *
               (size_t)format->bits_per_channel[0] +
               7u) /
        8u;
  }
  size_t bits = 0;
  for (uint8_t i = 0; i < format->channel_count && i < 8; i++) {
    bits += format->bits_per_channel[i];
  }
  return (bits + 7) / 8;
}

void gimg_raster_replace_owned_buffer(GIMG_Raster * raster, void * pixels,
    uint32_t width, uint32_t height, size_t stride_bytes) {
  if (!raster || !pixels) {
    return;
  }
  if (raster->ownership == GIMG_RASTER_OWNED && raster->pixels) {
    gimg_free(raster->allocator, raster->pixels);
  }
  raster->pixels = pixels;
  raster->ownership = GIMG_RASTER_OWNED;
  raster->width = width;
  raster->height = height;
  raster->stride_bytes = stride_bytes;
}

static size_t align_stride(size_t stride, uint8_t alignment) {
  if (alignment <= 1) {
    return stride;
  }
  return (stride + (size_t)(alignment - 1)) & ~(size_t)(alignment - 1);
}

GIMG_API GIMG_Result gimg_raster_create(uint32_t width, uint32_t height,
    const GIMG_Pixel_Format * format, GIMG_Raster_Ownership ownership,
    void * buffer, size_t stride_bytes, GIMG_Raster ** out_raster) {
  return gimg_raster_create_with_allocator(
      NULL, width, height, format, ownership, buffer, stride_bytes, out_raster);
}

GIMG_API GIMG_Result gimg_raster_create_with_allocator(
    const GIMG_Allocator * allocator, uint32_t width, uint32_t height,
    const GIMG_Pixel_Format * format, GIMG_Raster_Ownership ownership,
    void * buffer, size_t stride_bytes, GIMG_Raster ** out_raster) {
  if (!format || !out_raster || width == 0 || height == 0) {
    return GIMG_ERR_INTERNAL;
  }
  if (ownership != GIMG_RASTER_OWNED && ownership != GIMG_RASTER_BORROWED) {
    return GIMG_ERR_INTERNAL;
  }
  if (ownership == GIMG_RASTER_BORROWED && !buffer) {
    return GIMG_ERR_INTERNAL;
  }

  size_t bpp = gimg_raster_bytes_per_pixel(format);
  if (bpp == 0) {
    return GIMG_ERR_UNSUPPORTED;
  }

  size_t min_stride = width * bpp;
  uint8_t align = format->alignment ? format->alignment : 1;
  if (stride_bytes == 0) {
    stride_bytes = align_stride(min_stride, align);
  }
  else if (stride_bytes < min_stride) {
    return GIMG_ERR_INTERNAL;
  }

  allocator = gimg_alloc_or_default(allocator);
  GIMG_Raster * r = (GIMG_Raster *)gimg_malloc(allocator, sizeof(GIMG_Raster));
  if (!r) {
    return GIMG_ERR_OOM;
  }

  r->allocator = allocator;
  r->width = width;
  r->height = height;
  r->stride_bytes = stride_bytes;
  r->format = *format;
  r->ownership = ownership;
  gimg_color_info_default(&r->color_info);
  r->color_icc_owned = NULL;

  if (ownership == GIMG_RASTER_OWNED) {
    size_t total = stride_bytes * height;
    if (buffer) {
      // Caller transferred ownership; see raster.h OWNED + non-NULL.
      r->pixels = buffer;
    }
    else {
      r->pixels = gimg_calloc(allocator, 1, total);
      if (!r->pixels) {
        gimg_free(allocator, r);
        return GIMG_ERR_OOM;
      }
    }
  }
  else {
    r->pixels = buffer;
  }

  *out_raster = r;
  return GIMG_OK;
}

GIMG_API void gimg_raster_destroy(GIMG_Raster * raster) {
  if (!raster) {
    return;
  }
  const GIMG_Allocator * alloc = raster->allocator;
  if (raster->color_icc_owned) {
    gimg_free(alloc, raster->color_icc_owned);
    raster->color_icc_owned = NULL;
  }
  if (raster->ownership == GIMG_RASTER_OWNED && raster->pixels) {
    gimg_free(alloc, raster->pixels);
  }
  gimg_free(alloc, raster);
}

GIMG_API uint32_t gimg_raster_width(const GIMG_Raster * raster) {
  return raster ? raster->width : 0;
}

GIMG_API uint32_t gimg_raster_height(const GIMG_Raster * raster) {
  return raster ? raster->height : 0;
}

GIMG_API size_t gimg_raster_stride_bytes(const GIMG_Raster * raster) {
  return raster ? raster->stride_bytes : 0;
}

GIMG_API const GIMG_Pixel_Format * gimg_raster_format(
    const GIMG_Raster * raster) {
  return raster ? &raster->format : NULL;
}

GIMG_API GIMG_Raster_Ownership gimg_raster_ownership(
    const GIMG_Raster * raster) {
  return raster ? raster->ownership : GIMG_RASTER_OWNED;
}

GIMG_API void * gimg_raster_pixels(GIMG_Raster * raster) {
  return raster ? raster->pixels : NULL;
}

GIMG_API const void * gimg_raster_pixels_const(const GIMG_Raster * raster) {
  return raster ? raster->pixels : NULL;
}

GIMG_API const GIMG_Allocator * gimg_raster_allocator(
    const GIMG_Raster * raster) {
  return raster ? raster->allocator : NULL;
}

GIMG_API const GIMG_Color_Info * gimg_raster_color_info_const(
    const GIMG_Raster * raster) {
  return raster ? &raster->color_info : NULL;
}

GIMG_API GIMG_Result gimg_raster_set_color_info(GIMG_Raster * raster,
    const GIMG_Color_Info * info) {
  if (!raster || !info) {
    return GIMG_ERR_INTERNAL;
  }
  const GIMG_Allocator * alloc = raster->allocator;
  if (raster->color_icc_owned) {
    gimg_free(alloc, raster->color_icc_owned);
    raster->color_icc_owned = NULL;
  }
  raster->color_info = *info;
  if (info->icc_size > 0 && info->icc_bytes) {
    void * copy = gimg_malloc(alloc, info->icc_size);
    if (!copy) {
      gimg_color_info_default(&raster->color_info);
      return GIMG_ERR_OOM;
    }
    memcpy(copy, info->icc_bytes, info->icc_size);
    raster->color_icc_owned = copy;
    raster->color_info.icc_bytes = copy;
  }
  return GIMG_OK;
}

GIMG_API GIMG_Result gimg_raster_copy(const GIMG_Raster * src,
    GIMG_Raster ** out_raster) {
  return gimg_raster_copy_with_allocator(NULL, src, out_raster);
}

// Copy raster: create new raster with same dimensions and format (same
// validation as gimg_raster_create); copy pixels row-by-row so source stride
// is respected and destination gets library stride. Color info (including ICC
// if present) is deep-copied. Planar formats return GIMG_ERR_UNSUPPORTED
// (bpp == 0). Caller owns the returned raster.
GIMG_API GIMG_Result gimg_raster_copy_with_allocator(
    const GIMG_Allocator * allocator, const GIMG_Raster * src,
    GIMG_Raster ** out_raster) {
  if (!src || !out_raster) {
    return GIMG_ERR_INTERNAL;
  }
  *out_raster = NULL;
  const GIMG_Pixel_Format * fmt = gimg_raster_format(src);
  size_t bpp = gimg_raster_bytes_per_pixel(fmt);
  if (bpp == 0) {
    return GIMG_ERR_UNSUPPORTED; // Planar or invalid format.
  }
  uint32_t w = gimg_raster_width(src);
  uint32_t h = gimg_raster_height(src);
  GIMG_Result r = gimg_raster_create_with_allocator(allocator, w, h, fmt,
      GIMG_RASTER_OWNED, NULL, 0, out_raster);
  if (r != GIMG_OK) {
    return r;
  }
  size_t src_stride = gimg_raster_stride_bytes(src);
  size_t dst_stride = gimg_raster_stride_bytes(*out_raster);
  size_t row_bytes = (size_t)w * bpp;
  const unsigned char * sp =
      (const unsigned char *)gimg_raster_pixels_const(src);
  unsigned char * dp = (unsigned char *)gimg_raster_pixels(*out_raster);
  for (uint32_t y = 0; y < h; y++) {
    memcpy(dp, sp, row_bytes);
    sp += src_stride;
    dp += dst_stride;
  }
  const GIMG_Color_Info * ci = gimg_raster_color_info_const(src);
  if (ci) {
    r = gimg_raster_set_color_info(*out_raster, ci);
    if (r != GIMG_OK) {
      gimg_raster_destroy(*out_raster);
      *out_raster = NULL;
      return r;
    }
  }
  return GIMG_OK;
}

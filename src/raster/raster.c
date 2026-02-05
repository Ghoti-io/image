/**
 * @file
 *
 * Raster create/destroy and accessors.
 *
 * Copyright 2026 by Corey Pennycuff
 */

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

GIMG_API const GIMG_Pixel_Format GIMG_PIXEL_RGBA8 = gimg_pixel_rgba8;
GIMG_API const GIMG_Pixel_Format GIMG_PIXEL_RGBA16 = gimg_pixel_rgba16;
GIMG_API const GIMG_Pixel_Format GIMG_PIXEL_GRAY8 = gimg_pixel_gray8;
GIMG_API const GIMG_Pixel_Format GIMG_PIXEL_GRAY16 = gimg_pixel_gray16;

GIMG_API size_t gimg_raster_bytes_per_pixel(const GIMG_Pixel_Format * format) {
  if (!format || format->channel_count == 0) {
    return 0;
  }
  size_t bits = 0;
  for (uint8_t i = 0; i < format->channel_count && i < 8; i++) {
    bits += format->bits_per_channel[i];
  }
  return (bits + 7) / 8;
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
  return gimg_raster_create_with_allocator(NULL, width, height, format,
      ownership, buffer, stride_bytes, out_raster);
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
  GIMG_Raster * r =
      (GIMG_Raster *)gimg_malloc(allocator, sizeof(GIMG_Raster));
  if (!r) {
    return GIMG_ERR_OOM;
  }

  r->allocator = allocator;
  r->width = width;
  r->height = height;
  r->stride_bytes = stride_bytes;
  r->format = *format;
  r->ownership = ownership;

  if (ownership == GIMG_RASTER_OWNED) {
    size_t total = stride_bytes * height;
    r->pixels = gimg_calloc(allocator, 1, total);
    if (!r->pixels) {
      gimg_free(allocator, r);
      return GIMG_ERR_OOM;
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

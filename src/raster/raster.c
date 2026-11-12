/*
 * SPDX-License-Identifier: LGPL-3.0-only
 *
 * Copyright (C) 2026 Corey Pennycuff
 *
 * This file is part of Ghoti.io Image.
 *
 * Ghoti.io Image is free software: you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License version 3 as
 * published by the Free Software Foundation.
 *
 * Ghoti.io Image is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public
 * License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/**
 * @file
 *
 * Raster create/destroy and accessors.
 */

#include <ghoti.io/image/macros.h>
#include <ghoti.io/color/color.h>
#include <ghoti.io/image/raster.h>
#include <stdint.h>
#include <string.h>

#include "../core/alloc_internal.h"
#include "../core/safe_math_internal.h"
#include "raster_internal.h"

// Canonical format definitions.
static const GIMG_Pixel_Format gimg_pixel_rgba8 = {
    .channel_model = GIMG_CHANNEL_RGBA,
    .channel_type = GIMG_CHANNEL_UNORM,
    .layout = GIMG_LAYOUT_INTERLEAVED,
    .channel_count = 4,
    .bits_per_channel = {8, 8, 8, 8, 0, 0, 0, 0},
    .alignment = GIMG_DEFAULT_STRIDE_ALIGNMENT,
    .alpha_form = GIMG_ALPHA_STRAIGHT,
    ._reserved = {0},
};

static const GIMG_Pixel_Format gimg_pixel_rgba16 = {
    .channel_model = GIMG_CHANNEL_RGBA,
    .channel_type = GIMG_CHANNEL_UNORM,
    .layout = GIMG_LAYOUT_INTERLEAVED,
    .channel_count = 4,
    .bits_per_channel = {16, 16, 16, 16, 0, 0, 0, 0},
    .alignment = GIMG_DEFAULT_STRIDE_ALIGNMENT,
    .alpha_form = GIMG_ALPHA_STRAIGHT,
    ._reserved = {0},
};

static const GIMG_Pixel_Format gimg_pixel_gray8 = {
    .channel_model = GIMG_CHANNEL_GRAY,
    .channel_type = GIMG_CHANNEL_UNORM,
    .layout = GIMG_LAYOUT_INTERLEAVED,
    .channel_count = 1,
    .bits_per_channel = {8, 0, 0, 0, 0, 0, 0, 0},
    .alignment = GIMG_DEFAULT_STRIDE_ALIGNMENT,
    .alpha_form = GIMG_ALPHA_NONE,
    ._reserved = {0},
};

static const GIMG_Pixel_Format gimg_pixel_gray16 = {
    .channel_model = GIMG_CHANNEL_GRAY,
    .channel_type = GIMG_CHANNEL_UNORM,
    .layout = GIMG_LAYOUT_INTERLEAVED,
    .channel_count = 1,
    .bits_per_channel = {16, 0, 0, 0, 0, 0, 0, 0},
    .alignment = GIMG_DEFAULT_STRIDE_ALIGNMENT,
    .alpha_form = GIMG_ALPHA_NONE,
    ._reserved = {0},
};

static const GIMG_Pixel_Format gimg_pixel_gray12 = {
    .channel_model = GIMG_CHANNEL_GRAY,
    .channel_type = GIMG_CHANNEL_UNORM,
    .layout = GIMG_LAYOUT_INTERLEAVED,
    .channel_count = 1,
    .bits_per_channel = {12, 0, 0, 0, 0, 0, 0, 0},
    .alignment = GIMG_DEFAULT_STRIDE_ALIGNMENT,
    .alpha_form = GIMG_ALPHA_NONE,
    ._reserved = {0},
};

static const GIMG_Pixel_Format gimg_pixel_rgba12 = {
    .channel_model = GIMG_CHANNEL_RGBA,
    .channel_type = GIMG_CHANNEL_UNORM,
    .layout = GIMG_LAYOUT_INTERLEAVED,
    .channel_count = 4,
    .bits_per_channel = {12, 12, 12, 12, 0, 0, 0, 0},
    .alignment = GIMG_DEFAULT_STRIDE_ALIGNMENT,
    .alpha_form = GIMG_ALPHA_STRAIGHT,
    ._reserved = {0},
};

static const GIMG_Pixel_Format gimg_pixel_cmyk8 = {
    .channel_model = GIMG_CHANNEL_CMYK,
    .channel_type = GIMG_CHANNEL_UNORM,
    .layout = GIMG_LAYOUT_INTERLEAVED,
    .channel_count = 4,
    .bits_per_channel = {8, 8, 8, 8, 0, 0, 0, 0},
    .alignment = GIMG_DEFAULT_STRIDE_ALIGNMENT,
    .alpha_form = GIMG_ALPHA_NONE,
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
    .alpha_form = GIMG_ALPHA_NONE,
    ._reserved = {0},
};

static const GIMG_Pixel_Format gimg_pixel_cmyk16 = {
    .channel_model = GIMG_CHANNEL_CMYK,
    .channel_type = GIMG_CHANNEL_UNORM,
    .layout = GIMG_LAYOUT_INTERLEAVED,
    .channel_count = 4,
    .bits_per_channel = {16, 16, 16, 16, 0, 0, 0, 0},
    .alignment = GIMG_DEFAULT_STRIDE_ALIGNMENT,
    .alpha_form = GIMG_ALPHA_NONE,
    ._reserved = {0},
};

// Linear light that an integer cannot hold: LogLuv's 38 decades of
// luminance, and anything brighter than white.  Single precision because
// half precision stops at 65504 and 2^-24; nothing here reads or writes any
// other float width.
static const GIMG_Pixel_Format gimg_pixel_rgbaf32 = {
    .channel_model = GIMG_CHANNEL_RGBA,
    .channel_type = GIMG_CHANNEL_FLOAT,
    .layout = GIMG_LAYOUT_INTERLEAVED,
    .channel_count = 4,
    .bits_per_channel = {32, 32, 32, 32, 0, 0, 0, 0},
    .alignment = GIMG_DEFAULT_STRIDE_ALIGNMENT,
    .alpha_form = GIMG_ALPHA_STRAIGHT,
    ._reserved = {0},
};

static const GIMG_Pixel_Format gimg_pixel_grayf32 = {
    .channel_model = GIMG_CHANNEL_GRAY,
    .channel_type = GIMG_CHANNEL_FLOAT,
    .layout = GIMG_LAYOUT_INTERLEAVED,
    .channel_count = 1,
    .bits_per_channel = {32, 0, 0, 0, 0, 0, 0, 0},
    .alignment = GIMG_DEFAULT_STRIDE_ALIGNMENT,
    .alpha_form = GIMG_ALPHA_NONE,
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
GIMG_API const GIMG_Pixel_Format GIMG_PIXEL_RGBAF32 = gimg_pixel_rgbaf32;
GIMG_API const GIMG_Pixel_Format GIMG_PIXEL_GRAYF32 = gimg_pixel_grayf32;

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

  GIMG_Pixel_Format resolved = *format;
  if (resolved.alpha_form > GIMG_ALPHA_PREMULTIPLIED) {
    return GIMG_ERR_INVALID;
  }
  if (resolved.channel_model == GIMG_CHANNEL_RGBA) {
    // Zero is a struct from before the field existed; straight is what every
    // RGBA raster here has always been.
    if (resolved.alpha_form == GIMG_ALPHA_NONE) {
      resolved.alpha_form = GIMG_ALPHA_STRAIGHT;
    }
  }
  else if (resolved.alpha_form != GIMG_ALPHA_NONE) {
    return GIMG_ERR_INVALID;
  }
  format = &resolved;

  // A float channel is a C float.  Any other width would be read by every
  // integer path that switches on bits_per_channel - a sixteen-bit "float"
  // is a UNORM16 to all of them - so it is refused here, once.
  if (format->channel_type == GIMG_CHANNEL_FLOAT) {
    for (uint8_t i = 0; i < format->channel_count; i++) {
      if (gimg_pixel_format_channel_bits(format, i) != 32u) {
        return GIMG_ERR_UNSUPPORTED;
      }
    }
  }

  // The rest of the format, as a whole.  Every operation reads the channel
  // count the model implies and the depth of the first channel, so a format
  // that disagrees with itself - four channels of 16, 8, 8 and 8 bits under
  // an RGBA model, or two channels under CMYK - is one they would read and
  // write at a size the buffer does not have.
  if ((unsigned)format->channel_model >= (unsigned)GIMG_CHANNEL_COUNT ||
      (unsigned)format->channel_type >= (unsigned)GIMG_CHANNEL_TYPE_COUNT) {
    return GIMG_ERR_INVALID;
  }
  if (format->layout != GIMG_LAYOUT_INTERLEAVED) {
    // There is no planar storage for the operations to be correct about.
    return GIMG_ERR_UNSUPPORTED;
  }
  {
    unsigned expected = 0; // 0: any count
    switch (format->channel_model) {
    case GIMG_CHANNEL_GRAY:
    case GIMG_CHANNEL_INDEXED:
      expected = 1u;
      break;
    case GIMG_CHANNEL_RGB:
    case GIMG_CHANNEL_YCBCR:
    case GIMG_CHANNEL_LAB:
      expected = 3u;
      break;
    case GIMG_CHANNEL_RGBA:
    case GIMG_CHANNEL_CMYK:
      expected = 4u;
      break;
    default:
      break;
    }
    if (expected != 0u && format->channel_count != expected) {
      return GIMG_ERR_INVALID;
    }
    if (format->channel_count == 0u) {
      return GIMG_ERR_INVALID;
    }
  }
  {
    const unsigned depth = format->bits_per_channel[0];
    if (depth != 8u && depth != 12u && depth != 16u && depth != 32u) {
      // Whole-byte samples only; a depth below eight bits is not stored.
      return GIMG_ERR_UNSUPPORTED;
    }
    const unsigned n = format->channel_count < 8u ? format->channel_count : 8u;
    for (unsigned i = 1; i < n; i++) {
      if (format->bits_per_channel[i] != depth) {
        return GIMG_ERR_INVALID;
      }
    }
  }

  size_t bpp = gimg_raster_bytes_per_pixel(format);
  if (bpp == 0) {
    return GIMG_ERR_UNSUPPORTED;
  }

  // Every size below is checked, here and nowhere else.  Each decoder and
  // operation ends in this constructor, and a product that wraps is not a
  // large raster but a small one that says it is large: 2^31 x 2^31 RGBA8 is
  // 2^64 bytes, which is zero, and a zero-size request succeeds.
  size_t min_stride = 0;
  if (!gcu_safe_mul_size((size_t)width, bpp, &min_stride)) {
    return GIMG_ERR_LIMIT;
  }
  uint8_t align = format->alignment ? format->alignment : 1;
  if (stride_bytes == 0) {
    if (min_stride > SIZE_MAX - (size_t)(align - 1u)) {
      return GIMG_ERR_LIMIT;
    }
    stride_bytes = align_stride(min_stride, align);
  }
  else if (stride_bytes < min_stride) {
    return GIMG_ERR_INTERNAL;
  }
  size_t total = 0;
  if (!gcu_safe_mul_size(stride_bytes, (size_t)height, &total)) {
    return GIMG_ERR_LIMIT;
  }
  if (ownership == GIMG_RASTER_BORROWED) {
    // The operations read 16- and 32-bit samples through typed pointers, and a
    // misaligned access through one is undefined however it behaves at -O1.
    // The library's own buffers are aligned; a caller's must be too.
    const size_t sample =
        format->bits_per_channel[0] >= 32u ? 4u
        : format->bits_per_channel[0] >= 12u ? 2u
                                             : 1u;
    if ((sample > 1u) &&
        ((((uintptr_t)buffer) % sample) != 0u || (stride_bytes % sample) != 0u)) {
      return GIMG_ERR_INVALID;
    }
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
  gcol_color_info_default(&r->color_info);
  r->color_icc_owned = NULL;
  r->color_icc_linked_path_owned = NULL;

  if (ownership == GIMG_RASTER_OWNED) {
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
  if (raster->color_icc_linked_path_owned) {
    gimg_free(alloc, raster->color_icc_linked_path_owned);
    raster->color_icc_linked_path_owned = NULL;
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

GIMG_API const GCOL_Color_Info * gimg_raster_color_info_const(
    const GIMG_Raster * raster) {
  return raster ? &raster->color_info : NULL;
}

GIMG_API GIMG_Result gimg_raster_set_color_info(GIMG_Raster * raster,
    const GCOL_Color_Info * info) {
  if (!raster || !info) {
    return GIMG_ERR_INTERNAL;
  }
  const GIMG_Allocator * alloc = raster->allocator;
  // Both copies are made before anything the raster holds is released.
  // `info` may be this raster's own description, copied out and edited, in
  // which case its profile and path pointers are the blocks about to be
  // freed.  It also means a failed allocation leaves the raster as it was.
  void * icc_copy = NULL;
  char * path_copy = NULL;
  if (info->icc_size > 0 && info->icc_bytes) {
    icc_copy = gimg_malloc(alloc, info->icc_size);
    if (!icc_copy) {
      return GIMG_ERR_OOM;
    }
    memcpy(icc_copy, info->icc_bytes, info->icc_size);
  }
  // The linked path is copied for the same reason the profile is: a raster
  // outlives the document that read it.
  if (info->icc_linked_path) {
    size_t len = strlen(info->icc_linked_path);
    path_copy = (char *)gimg_malloc(alloc, len + 1u);
    if (!path_copy) {
      gimg_free(alloc, icc_copy);
      return GIMG_ERR_OOM;
    }
    memcpy(path_copy, info->icc_linked_path, len + 1u);
  }
  GCOL_Color_Info next = *info;
  next.icc_bytes = icc_copy;
  next.icc_size = icc_copy ? info->icc_size : 0u;
  next.icc_linked_path = path_copy;
  if (raster->color_icc_owned) {
    gimg_free(alloc, raster->color_icc_owned);
  }
  if (raster->color_icc_linked_path_owned) {
    gimg_free(alloc, raster->color_icc_linked_path_owned);
  }
  raster->color_icc_owned = icc_copy;
  raster->color_icc_linked_path_owned = path_copy;
  raster->color_info = next;
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
  const GCOL_Color_Info * ci = gimg_raster_color_info_const(src);
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

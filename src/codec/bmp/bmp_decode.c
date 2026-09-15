/**
 * @file
 *
 * BMP decode: stored pixel bytes -> RGBA8 raster.
 *
 * Copyright 2026 by Corey Pennycuff
 *
 * --- Internal algorithms and design ---
 *
 * Row order: BMP stores rows bottom-up unless the header declared a negative
 * height.  We write into the raster top-down in both cases, choosing the
 * destination row rather than reversing the buffer afterwards.
 *
 * Bit depths: 1/2/4/8 index a palette; 16 and 32 extract channels through the
 * masks resolved at load time; 24 is a fixed BGR triple.  Every palette index
 * is checked against the entry count the file actually provided, so a
 * truncated palette cannot be read past.
 *
 * 32-bit alpha: BI_RGB leaves the high byte undefined, and writers split
 * roughly evenly between storing alpha there and storing zero.  Honoring a
 * zero byte would make the whole image transparent, so for BI_RGB we scan the
 * high bytes first and treat the channel as opaque when every one of them is
 * zero.  An explicit BI_BITFIELDS alpha mask is always honored as-is.
 *
 * RLE: runs are clipped to the row and the decoder refuses to advance past
 * the last row, so a hostile stream cannot write outside the raster.  Pixels
 * never reached by the encoded data stay at the zeroed initial value, which
 * is what the format specifies for a delta that skips them.
 */

#include <ghoti.io/image/codec.h>
#include <ghoti.io/image/core.h>
#include <ghoti.io/image/doc.h>
#include <ghoti.io/image/raster.h>
#include <string.h>

#include "../../container/doc_internal.h"
#include "../../core/alloc_internal.h"
#include "../../core/safe_math_internal.h"
#include "../codec_internal.h"
#include "bmp_internal.h"

/** Expand a channel sample from its mask width to 8 bits. */
static uint8_t bmp_scale_channel(
    uint32_t pixel, const gimg_bmp_channel_mask_t * m) {
  if (!m->mask || !m->max) {
    return 0;
  }
  uint32_t value = (pixel & m->mask) >> m->shift;
  if (m->max == 255u) {
    return (uint8_t)value;
  }
  // Round rather than truncate so that the maximum input maps to 255 and the
  // midpoint does not drift downward.
  return (uint8_t)(((value * 255u) + (m->max / 2u)) / m->max);
}

/** Read a little-endian pixel of `bytes` width (2 or 4). */
static uint32_t bmp_read_pixel(const unsigned char * p, unsigned int bytes) {
  uint32_t value = 0;
  for (unsigned int i = 0; i < bytes; i++) {
    value |= (uint32_t)p[i] << (8u * i);
  }
  return value;
}

/** Destination row in a top-down raster for source row `y`. */
static uint32_t bmp_dest_row(
    const gimg_bmp_header_t * header, uint32_t y) {
  return header->top_down ? y : (header->height - 1u - y);
}

/** Write one opaque RGB pixel. */
static void bmp_put_rgb(
    uint8_t * row, uint32_t x, uint8_t r, uint8_t g, uint8_t b) {
  uint8_t * px = row + ((size_t)x * 4u);
  px[0] = r;
  px[1] = g;
  px[2] = b;
  px[3] = 255u;
}

// ---------------------------------------------------------------------------
// Uncompressed
// ---------------------------------------------------------------------------

/**
 * Decide whether a 32-bit BI_RGB image's high byte carries alpha.
 *
 * Returns true when at least one pixel has a non-zero high byte.
 */
static bool bmp_rgb32_has_alpha(const gimg_bmp_doc_state_t * state,
    size_t stride) {
  const gimg_bmp_header_t * h = &state->header;
  for (uint32_t y = 0; y < h->height; y++) {
    const unsigned char * src = state->pixels + ((size_t)y * stride);
    for (uint32_t x = 0; x < h->width; x++) {
      uint32_t pixel = bmp_read_pixel(src + ((size_t)x * 4u), 4u);
      if (bmp_scale_channel(pixel, &h->alpha) != 0) {
        return true;
      }
    }
  }
  return false;
}

static GIMG_Result bmp_decode_uncompressed(
    const gimg_bmp_doc_state_t * state, GIMG_Raster * raster) {
  const gimg_bmp_header_t * h = &state->header;

  size_t stride;
  GIMG_Result r = gimg_bmp_row_stride(h->width, h->bit_count, &stride);
  if (r != GIMG_OK) {
    return r;
  }
  // bmp_read_pixels() sized the buffer from this same computation, so a
  // mismatch here means the state was built inconsistently.
  size_t required;
  if (!gimg_safe_mul_size(stride, (size_t)h->height, &required) ||
      required > state->pixels_size) {
    return GIMG_ERR_INTERNAL;
  }

  uint8_t * dest = (uint8_t *)gimg_raster_pixels(raster);
  size_t dest_stride = gimg_raster_stride_bytes(raster);

  // For BI_RGB at 32bpp the high byte is only alpha if something set it.
  bool use_alpha = h->alpha.mask != 0;
  if (use_alpha && h->bit_count == 32 &&
      h->compression == GIMG_BMP_BI_RGB) {
    use_alpha = bmp_rgb32_has_alpha(state, stride);
  }

  for (uint32_t y = 0; y < h->height; y++) {
    const unsigned char * src = state->pixels + ((size_t)y * stride);
    uint8_t * row = dest + ((size_t)bmp_dest_row(h, y) * dest_stride);

    switch (h->bit_count) {
      case 1:
      case 2:
      case 4:
      case 8: {
        unsigned int bits = h->bit_count;
        unsigned int mask = (1u << bits) - 1u;
        for (uint32_t x = 0; x < h->width; x++) {
          size_t bit_index = (size_t)x * bits;
          unsigned int shift = (unsigned int)(8u - bits - (bit_index % 8u));
          unsigned int index =
              (unsigned int)((src[bit_index / 8u] >> shift) & mask);
          if (index >= state->palette_count) {
            return GIMG_ERR_CORRUPT;
          }
          const gimg_bmp_palette_entry_t * e = &state->palette[index];
          bmp_put_rgb(row, x, e->r, e->g, e->b);
        }
        break;
      }
      case 24: {
        for (uint32_t x = 0; x < h->width; x++) {
          const unsigned char * px = src + ((size_t)x * 3u);
          bmp_put_rgb(row, x, px[2], px[1], px[0]);
        }
        break;
      }
      case 16:
      case 32: {
        unsigned int bytes = h->bit_count / 8u;
        for (uint32_t x = 0; x < h->width; x++) {
          uint32_t pixel = bmp_read_pixel(src + ((size_t)x * bytes), bytes);
          uint8_t * out = row + ((size_t)x * 4u);
          out[0] = bmp_scale_channel(pixel, &h->red);
          out[1] = bmp_scale_channel(pixel, &h->green);
          out[2] = bmp_scale_channel(pixel, &h->blue);
          out[3] = use_alpha ? bmp_scale_channel(pixel, &h->alpha) : 255u;
        }
        break;
      }
      default:
        return GIMG_ERR_UNSUPPORTED;
    }
  }

  return GIMG_OK;
}

// ---------------------------------------------------------------------------
// RLE
// ---------------------------------------------------------------------------

/** Plot one palette index, clipping anything outside the row. */
static GIMG_Result bmp_rle_plot(const gimg_bmp_doc_state_t * state,
    uint8_t * dest, size_t dest_stride, uint32_t x, uint32_t y,
    unsigned int index) {
  const gimg_bmp_header_t * h = &state->header;
  if (index >= state->palette_count) {
    return GIMG_ERR_CORRUPT;
  }
  if (x >= h->width || y >= h->height) {
    // Encoders legitimately emit runs that overhang the row; the excess is
    // discarded rather than treated as corruption.
    return GIMG_OK;
  }
  uint8_t * row = dest + ((size_t)bmp_dest_row(h, y) * dest_stride);
  const gimg_bmp_palette_entry_t * e = &state->palette[index];
  bmp_put_rgb(row, x, e->r, e->g, e->b);
  return GIMG_OK;
}

static GIMG_Result bmp_decode_rle(
    const gimg_bmp_doc_state_t * state, GIMG_Raster * raster) {
  const gimg_bmp_header_t * h = &state->header;
  bool rle4 = h->compression == GIMG_BMP_BI_RLE4;

  uint8_t * dest = (uint8_t *)gimg_raster_pixels(raster);
  size_t dest_stride = gimg_raster_stride_bytes(raster);

  const unsigned char * p = state->pixels;
  const unsigned char * end = state->pixels + state->pixels_size;

  uint32_t x = 0;
  uint32_t y = 0;

  while (p + 2 <= end) {
    unsigned int count = *p++;
    unsigned int value = *p++;

    if (count) {
      // Encoded run: `count` pixels of one color (RLE8) or of two alternating
      // nibbles (RLE4).
      for (unsigned int i = 0; i < count; i++) {
        unsigned int index = rle4
            ? ((i & 1u) ? (value & 0x0Fu) : ((value >> 4) & 0x0Fu))
            : value;
        GIMG_Result r =
            bmp_rle_plot(state, dest, dest_stride, x, y, index);
        if (r != GIMG_OK) {
          return r;
        }
        x++;
      }
      continue;
    }

    // Escape.
    if (value == 0) {
      // End of line.
      x = 0;
      y++;
      if (y >= h->height) {
        break;
      }
      continue;
    }
    if (value == 1) {
      // End of bitmap.
      break;
    }
    if (value == 2) {
      // Delta: skip dx right and dy down.  Skipped pixels keep the raster's
      // initial value.
      if (p + 2 > end) {
        return GIMG_ERR_CORRUPT;
      }
      x += *p++;
      y += *p++;
      if (y >= h->height) {
        break;
      }
      continue;
    }

    // Absolute run of `value` literal indices, padded to a 16-bit boundary.
    unsigned int n = value;
    size_t encoded = rle4 ? (size_t)((n + 1u) / 2u) : (size_t)n;
    size_t padded = encoded + (encoded & 1u);
    if (p + padded > end) {
      return GIMG_ERR_CORRUPT;
    }
    for (unsigned int i = 0; i < n; i++) {
      unsigned int index;
      if (rle4) {
        unsigned char byte = p[i / 2u];
        index = (i & 1u) ? (byte & 0x0Fu) : ((byte >> 4) & 0x0Fu);
      }
      else {
        index = p[i];
      }
      GIMG_Result r = bmp_rle_plot(state, dest, dest_stride, x, y, index);
      if (r != GIMG_OK) {
        return r;
      }
      x++;
    }
    p += padded;
  }

  return GIMG_OK;
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

GIMG_Result gimg_bmp_decode(GIMG_Codec * codec, const GIMG_Item * item,
    const GIMG_Decode_Options * options, GIMG_Raster ** out_raster) {
  if (!codec || !item || !out_raster) {
    return GIMG_ERR_INTERNAL;
  }
  *out_raster = NULL;

  const GIMG_Doc * doc = item->doc;
  if (!doc || !doc->codec_private) {
    return GIMG_ERR_UNSUPPORTED;
  }
  // Accept the codec that loaded the document, by pointer or by name; the
  // save path can reach here with a registry lookup rather than the original
  // pointer.
  const GIMG_Codec * doc_codec = doc->loaded_by_codec;
  if (!doc_codec ||
      (doc_codec != codec &&
          (!gimg_codec_name(doc_codec) || !gimg_codec_name(codec) ||
              strcmp(gimg_codec_name(doc_codec), gimg_codec_name(codec)) !=
                  0))) {
    return GIMG_ERR_UNSUPPORTED;
  }
  if (item->index != 0) {
    // BMP holds a single image.
    return GIMG_ERR_UNSUPPORTED;
  }

  const gimg_bmp_doc_state_t * state =
      (const gimg_bmp_doc_state_t *)doc->codec_private;
  const gimg_bmp_header_t * h = &state->header;

  const GIMG_Limits * limits = options ? options->limits : NULL;
  if (limits && limits->max_decoded_pixels) {
    size_t pixel_count;
    if (!gimg_safe_mul_size(
            (size_t)h->width, (size_t)h->height, &pixel_count) ||
        pixel_count > limits->max_decoded_pixels) {
      return GIMG_ERR_LIMIT;
    }
  }

  const GIMG_Allocator * alloc = gimg_alloc_or_default(codec->allocator);
  GIMG_Raster * raster = NULL;
  GIMG_Result r = gimg_raster_create_with_allocator(alloc, h->width, h->height,
      &GIMG_PIXEL_RGBA8, GIMG_RASTER_OWNED, NULL, 0, &raster);
  if (r != GIMG_OK) {
    return r;
  }

  bool is_rle = h->compression == GIMG_BMP_BI_RLE8 ||
      h->compression == GIMG_BMP_BI_RLE4;
  r = is_rle ? bmp_decode_rle(state, raster)
             : bmp_decode_uncompressed(state, raster);
  if (r != GIMG_OK) {
    gimg_raster_destroy(raster);
    return r;
  }

  *out_raster = raster;
  return GIMG_OK;
}

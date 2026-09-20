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
 * roughly evenly between storing alpha there and storing zero.  There is
 * nothing in the file that tells the two apart, so the default is to ignore
 * the byte and decode opaque, which is what the format says it means and what
 * every other decoder does.  GIMG_Load_Options.bmp_rgb32_alpha asks instead
 * for the heuristic - read it as alpha when any pixel sets it - for a caller
 * whose files are known to carry it.  An explicit alpha mask, from
 * BI_BITFIELDS, BI_ALPHABITFIELDS or a V3 or later header, is always honored
 * as written and is not affected by either.
 *
 * RLE: RLE8 and RLE4 name palette entries; the OS/2 RLE24 carries a BGR
 * triple per pixel and uses no palette.  All three share one escape
 * structure, so they share one loop.  Runs are clipped to the row and the
 * decoder refuses to advance past the last row, so a hostile stream cannot
 * write outside the raster.  Pixels never reached by the encoded data stay at
 * the zeroed initial value, which is what the format specifies for a delta
 * that skips them.
 */

#include <ghoti.io/image/macros.h>
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
  if (!gcu_safe_mul_size(stride, (size_t)h->height, &required) ||
      required > state->pixels_size) {
    return GIMG_ERR_INTERNAL;
  }

  uint8_t * dest = (uint8_t *)gimg_raster_pixels(raster);
  size_t dest_stride = gimg_raster_stride_bytes(raster);

  // BI_RGB at 32bpp does not define the fourth byte, so by default it says
  // nothing about transparency and the image decodes opaque.  A caller who
  // knows their files put alpha there asks for the heuristic instead.
  bool use_alpha = h->alpha.mask != 0;
  if (use_alpha && h->bit_count == 32 &&
      h->compression == GIMG_BMP_COMP_RGB) {
    use_alpha = state->rgb32_alpha == GIMG_BMP_RGB32_ALPHA_HEURISTIC &&
        bmp_rgb32_has_alpha(state, stride);
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

/** Plot one pixel, clipping anything outside the raster. */
static void bmp_rle_put(const gimg_bmp_header_t * h, uint8_t * dest,
    size_t dest_stride, uint32_t x, uint32_t y, uint8_t r, uint8_t g,
    uint8_t b) {
  if (x >= h->width || y >= h->height) {
    // Encoders legitimately emit runs that overhang the row; the excess is
    // discarded rather than treated as corruption.
    return;
  }
  bmp_put_rgb(dest + ((size_t)bmp_dest_row(h, y) * dest_stride), x, r, g, b);
}

/** Plot one palette index, refusing an index the file has no entry for. */
static GIMG_Result bmp_rle_plot(const gimg_bmp_doc_state_t * state,
    uint8_t * dest, size_t dest_stride, uint32_t x, uint32_t y,
    unsigned int index) {
  if (index >= state->palette_count) {
    return GIMG_ERR_CORRUPT;
  }
  const gimg_bmp_palette_entry_t * e = &state->palette[index];
  bmp_rle_put(&state->header, dest, dest_stride, x, y, e->r, e->g, e->b);
  return GIMG_OK;
}

/**
 * Decode a run-length encoded bitmap: RLE8, RLE4, or the OS/2 RLE24.
 *
 * All three share the escape structure - a zero count introduces end-of-line,
 * end-of-bitmap, a delta, or an absolute run - and differ only in what a
 * "pixel" costs in the stream.  RLE8 and RLE4 name palette entries; RLE24
 * carries a BGR triple per pixel and uses no palette at all, which is why the
 * plotting is split into an index form and a literal form.
 */
static GIMG_Result bmp_decode_rle(
    const gimg_bmp_doc_state_t * state, GIMG_Raster * raster) {
  const gimg_bmp_header_t * h = &state->header;
  bool rle4 = h->compression == GIMG_BMP_COMP_RLE4;
  bool rle24 = h->compression == GIMG_BMP_COMP_RLE24;

  uint8_t * dest = (uint8_t *)gimg_raster_pixels(raster);
  size_t dest_stride = gimg_raster_stride_bytes(raster);

  const unsigned char * p = state->pixels;
  const unsigned char * end = state->pixels + state->pixels_size;

  uint32_t x = 0;
  uint32_t y = 0;

  while (p < end) {
    unsigned int count = *p++;

    if (count) {
      // Encoded run.  RLE8 repeats one index, RLE4 alternates the two nibbles
      // of the value byte, RLE24 repeats one BGR triple.
      if (rle24) {
        if (end - p < 3) {
          return GIMG_ERR_CORRUPT;
        }
        uint8_t b = p[0], g = p[1], rr = p[2];
        p += 3;
        for (unsigned int i = 0; i < count; i++, x++) {
          bmp_rle_put(h, dest, dest_stride, x, y, rr, g, b);
        }
        continue;
      }
      if (p >= end) {
        return GIMG_ERR_CORRUPT;
      }
      unsigned int value = *p++;
      for (unsigned int i = 0; i < count; i++, x++) {
        unsigned int index = rle4
            ? ((i & 1u) ? (value & 0x0Fu) : ((value >> 4) & 0x0Fu))
            : value;
        GIMG_Result r = bmp_rle_plot(state, dest, dest_stride, x, y, index);
        if (r != GIMG_OK) {
          return r;
        }
      }
      continue;
    }

    // Escape.
    if (p >= end) {
      return GIMG_ERR_CORRUPT;
    }
    unsigned int value = *p++;

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
      if (end - p < 2) {
        return GIMG_ERR_CORRUPT;
      }
      x += *p++;
      y += *p++;
      if (y >= h->height) {
        break;
      }
      continue;
    }

    // Absolute run of `value` literal pixels, padded to a 16-bit boundary.
    unsigned int n = value;
    size_t encoded = rle24 ? (size_t)n * 3u
                           : (rle4 ? (size_t)((n + 1u) / 2u) : (size_t)n);
    size_t padded = encoded + (encoded & 1u);
    if ((size_t)(end - p) < padded) {
      return GIMG_ERR_CORRUPT;
    }
    for (unsigned int i = 0; i < n; i++, x++) {
      if (rle24) {
        bmp_rle_put(h, dest, dest_stride, x, y, p[(i * 3u) + 2u],
            p[(i * 3u) + 1u], p[i * 3u]);
        continue;
      }
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

  // BI_JPEG and BI_PNG: the "pixel data" is a whole JPEG or PNG, which load
  // handed to that format's own codec.  There is nothing for this one to
  // decode, only a document to ask.  The BMP header's biWidth and biBitCount
  // describe the image it stands in for; the stream inside is the image, and
  // where the two disagree the stream is what the pixels actually are.
  if (gimg_bmp_is_embedded(h->compression)) {
    if (!state->embedded) {
      return GIMG_ERR_INTERNAL;
    }
    return gimg_item_decode(
        gimg_doc_item(state->embedded, 0), options, out_raster);
  }

  const GIMG_Limits * limits = options ? options->limits : NULL;
  if (limits && limits->max_decoded_pixels) {
    size_t pixel_count;
    if (!gcu_safe_mul_size(
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

  r = gimg_bmp_is_rle(h->compression)
      ? bmp_decode_rle(state, raster)
      : bmp_decode_uncompressed(state, raster);
  if (r != GIMG_OK) {
    gimg_raster_destroy(raster);
    return r;
  }

  *out_raster = raster;
  return GIMG_OK;
}

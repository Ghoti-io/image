/**
 * @file
 *
 * GIF decode: expand one frame's LZW codes to palette indices, then composite
 * that frame and every frame before it onto the logical screen.
 *
 * A GIF frame is not a picture on its own.  It is a patch at some position on
 * a canvas, and what the viewer sees depends on the frames that came before
 * and on how each of those was disposed of afterwards (89a 23).  Decoding
 * item N therefore replays items 0 through N rather than reading N alone -
 * the same arrangement the APNG path uses, for the same reason.
 *
 * See documentation/formats/gif.md.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <ghoti.io/image/macros.h>
#include <ghoti.io/image/codec.h>
#include <ghoti.io/image/core.h>
#include <ghoti.io/image/doc.h>
#include <ghoti.io/image/raster.h>
#include <string.h>

#include <ghoti.io/compress/compress.h>
#include <ghoti.io/compress/options.h>
#include <ghoti.io/compress/registry.h>

#include "../../container/doc_internal.h"
#include "../../core/alloc_internal.h"
#include "../../core/safe_math_internal.h"
#include "../codec_internal.h"
#include "gif_internal.h"

// ---------------------------------------------------------------------------
// LZW
// ---------------------------------------------------------------------------

/**
 * The four interlace passes: the first row each one starts at, and the gap
 * between its rows (89a 20).
 */
static const struct {
  uint16_t start;
  uint16_t step;
} gif_interlace_passes[4] = {{0u, 8u}, {4u, 8u}, {2u, 4u}, {1u, 2u}};

GIMG_Result gimg_gif_expand_lzw(const gimg_gif_frame_t * frame,
    const GIMG_Allocator * alloc, unsigned char * out, size_t out_size) {
  if (!frame || !out) {
    return GIMG_ERR_INTERNAL;
  }
  size_t pixels = 0;
  if (!gcu_safe_mul_size((size_t)frame->width, (size_t)frame->height,
          &pixels) ||
      pixels != out_size) {
    return GIMG_ERR_INTERNAL;
  }
  if (!frame->lzw || frame->lzw_size == 0u) {
    return GIMG_ERR_CORRUPT;
  }

  gcomp_options_t * opts = NULL;
  if (gcomp_options_create(&opts) != GCOMP_OK) {
    return GIMG_ERR_OOM;
  }
  if (gcomp_options_set_string(opts, "lzw.format", "gif") != GCOMP_OK ||
      gcomp_options_set_uint64(
          opts, "lzw.lit_width", frame->lzw_min_code_size) != GCOMP_OK) {
    gcomp_options_destroy(opts);
    return GIMG_ERR_INTERNAL;
  }

  // The scratch buffer is one byte longer than the pixels asked for, for two
  // separate reasons.  A GIF may carry more codes than the image has pixels,
  // and the extra byte is what makes that visible as a length rather than
  // being silently truncated to a correct-looking count.  It is also what the
  // LZW method needs: handed a buffer of exactly the output size it returns
  // GCOMP_ERR_LIMIT even though the data fits, which zlib, deflate, rle and
  // zstd in the same library do not do.
  size_t capacity = 0;
  if (!gcu_safe_add_size(pixels, 1u, &capacity)) {
    gcomp_options_destroy(opts);
    return GIMG_ERR_LIMIT;
  }
  unsigned char * raw = (unsigned char *)gimg_malloc(alloc, capacity);
  if (!raw) {
    gcomp_options_destroy(opts);
    return GIMG_ERR_OOM;
  }

  size_t written = 0;
  gcomp_status_t gs = gcomp_decode_buffer(gcomp_registry_default(), "lzw",
      opts, frame->lzw, frame->lzw_size, raw, capacity, &written);
  gcomp_options_destroy(opts);

  // A stream that runs out early is corrupt.  One that carries more than the
  // image needs is not: writers pad, and every decoder stops at the pixel
  // count the descriptor gave.  Only a shortfall is refused.
  if (gs != GCOMP_OK && written < pixels) {
    gimg_free(alloc, raw);
    return GIMG_ERR_CORRUPT;
  }
  if (written < pixels) {
    gimg_free(alloc, raw);
    return GIMG_ERR_CORRUPT;
  }

  if (!frame->interlaced) {
    memcpy(out, raw, pixels);
    gimg_free(alloc, raw);
    return GIMG_OK;
  }

  // Interlaced rows arrive in pass order; they are put where they belong here
  // so that nothing downstream has to know the frame was interlaced at all.
  size_t src_row = 0;
  for (size_t pass = 0; pass < 4u; pass++) {
    for (uint32_t y = gif_interlace_passes[pass].start; y < frame->height;
         y += gif_interlace_passes[pass].step) {
      memcpy(out + (size_t)y * frame->width,
          raw + src_row * frame->width, frame->width);
      src_row++;
    }
  }
  gimg_free(alloc, raw);
  return GIMG_OK;
}

// ---------------------------------------------------------------------------
// Compositing
// ---------------------------------------------------------------------------

/** Paint one frame's pixels onto the canvas, honouring its transparency. */
static void gif_paint(const gimg_gif_doc_state_t * state,
    const gimg_gif_frame_t * frame, const unsigned char * indices,
    uint8_t * canvas, size_t stride) {
  for (uint32_t y = 0; y < frame->height; y++) {
    uint32_t cy = (uint32_t)frame->top + y;
    if (cy >= state->canvas_height) {
      break;
    }
    uint8_t * row = canvas + (size_t)cy * stride;
    for (uint32_t x = 0; x < frame->width; x++) {
      uint32_t cx = (uint32_t)frame->left + x;
      if (cx >= state->canvas_width) {
        break;
      }
      uint8_t index = indices[(size_t)y * frame->width + x];
      if (frame->has_transparency && index == frame->transparent_index) {
        // Transparent means "leave what is already here", which is what the
        // canvas already holds - so the pixel is skipped, not written clear.
        continue;
      }
      uint8_t * px = row + (size_t)cx * 4u;
      if (index >= frame->palette_count) {
        // The code stream can name an index the colour table does not have:
        // the code width is set independently of the table size, so a 4-entry
        // table and an 8-bit code size can produce 255.  There is no colour to
        // draw, so nothing is drawn.  Refusing the file instead would reject
        // images every other decoder displays.
        continue;
      }
      px[0] = frame->palette[index].r;
      px[1] = frame->palette[index].g;
      px[2] = frame->palette[index].b;
      px[3] = 0xFFu;
    }
  }
}

/** Clear a frame's rectangle back to transparent (the disposal case). */
static void gif_clear_rect(const gimg_gif_doc_state_t * state,
    const gimg_gif_frame_t * frame, uint8_t * canvas, size_t stride) {
  for (uint32_t y = 0; y < frame->height; y++) {
    uint32_t cy = (uint32_t)frame->top + y;
    if (cy >= state->canvas_height) {
      break;
    }
    uint8_t * row = canvas + (size_t)cy * stride;
    for (uint32_t x = 0; x < frame->width; x++) {
      uint32_t cx = (uint32_t)frame->left + x;
      if (cx >= state->canvas_width) {
        break;
      }
      memset(row + (size_t)cx * 4u, 0, 4u);
    }
  }
}

GIMG_Result gimg_gif_decode(GIMG_Codec * codec, const GIMG_Item * item,
    const GIMG_Decode_Options * options, GIMG_Raster ** out_raster) {
  if (!codec || !item || !out_raster) {
    return GIMG_ERR_INTERNAL;
  }
  *out_raster = NULL;

  const GIMG_Doc * doc = item->doc;
  if (!doc || !doc->codec_private) {
    return GIMG_ERR_INTERNAL;
  }
  const gimg_gif_doc_state_t * state =
      (const gimg_gif_doc_state_t *)doc->codec_private;
  size_t index = (size_t)item->index;
  if (index >= state->frame_count) {
    return GIMG_ERR_INTERNAL;
  }

  const GIMG_Limits * limits = options ? options->limits : NULL;
  size_t canvas_pixels = 0;
  if (!gcu_safe_mul_size((size_t)state->canvas_width,
          (size_t)state->canvas_height, &canvas_pixels)) {
    return GIMG_ERR_LIMIT;
  }
  if (limits && limits->max_decoded_pixels &&
      canvas_pixels > limits->max_decoded_pixels) {
    return GIMG_ERR_LIMIT;
  }

  const GIMG_Allocator * alloc = gimg_alloc_or_default(codec->allocator);
  GIMG_Raster * raster = NULL;
  GIMG_Result r = gimg_raster_create_with_allocator(alloc, state->canvas_width,
      state->canvas_height, &GIMG_PIXEL_RGBA8, GIMG_RASTER_OWNED, NULL, 0,
      &raster);
  if (r != GIMG_OK) {
    return r;
  }

  uint8_t * canvas = (uint8_t *)gimg_raster_pixels(raster);
  size_t stride = gimg_raster_stride_bytes(raster);
  // The logical screen starts empty rather than filled with the background
  // colour.  The specification names a background index (89a 18), but the
  // viewers everyone's files were authored against ignore it and start
  // transparent; filling it would put a colour on screen that no other
  // decoder shows.  The index is kept in the document state for a caller that
  // wants it.
  for (uint32_t y = 0; y < state->canvas_height; y++) {
    memset(canvas + (size_t)y * stride, 0, (size_t)state->canvas_width * 4u);
  }

  uint8_t * saved = NULL;
  unsigned char * indices = NULL;

  for (size_t i = 0; i <= index; i++) {
    const gimg_gif_frame_t * frame = &state->frames[i];
    size_t pixels = (size_t)frame->width * (size_t)frame->height;

    unsigned char * grown =
        (unsigned char *)gimg_realloc(alloc, indices, pixels);
    if (!grown) {
      r = GIMG_ERR_OOM;
      break;
    }
    indices = grown;

    r = gimg_gif_expand_lzw(frame, alloc, indices, pixels);
    if (r != GIMG_OK) {
      break;
    }

    // Only the frame that is about to be replaced needs keeping, and only
    // when the next disposal asks for it, so the copy is made here rather
    // than kept for every frame.
    if (i < index && frame->disposal == GIMG_GIF_DISPOSAL_PREVIOUS) {
      if (!saved) {
        saved = (uint8_t *)gimg_malloc(alloc, stride * state->canvas_height);
        if (!saved) {
          r = GIMG_ERR_OOM;
          break;
        }
      }
      memcpy(saved, canvas, stride * state->canvas_height);
    }

    gif_paint(state, frame, indices, canvas, stride);

    if (i < index) {
      if (frame->disposal == GIMG_GIF_DISPOSAL_BACKGROUND) {
        gif_clear_rect(state, frame, canvas, stride);
      }
      else if (frame->disposal == GIMG_GIF_DISPOSAL_PREVIOUS && saved) {
        memcpy(canvas, saved, stride * state->canvas_height);
      }
    }
  }

  gimg_free(alloc, indices);
  gimg_free(alloc, saved);

  if (r != GIMG_OK) {
    gimg_raster_destroy(raster);
    return r;
  }

  *out_raster = raster;
  return GIMG_OK;
}

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

/**
 * Apply a frame's disposal, which is what turns the canvas the frame was
 * drawn on into the canvas the next frame starts from (89a 23).
 *
 * `saved` is the canvas as it stood before this frame was painted, and is
 * only consulted for GIMG_GIF_DISPOSAL_PREVIOUS.
 */
static void gif_dispose(const gimg_gif_doc_state_t * state,
    const gimg_gif_frame_t * frame, uint8_t * canvas, size_t stride,
    const uint8_t * saved, size_t canvas_bytes) {
  if (frame->disposal == GIMG_GIF_DISPOSAL_BACKGROUND) {
    gif_clear_rect(state, frame, canvas, stride);
  }
  else if (frame->disposal == GIMG_GIF_DISPOSAL_PREVIOUS && saved) {
    memcpy(canvas, saved, canvas_bytes);
  }
}

/**
 * Record the canvas the frame after `index` starts from.
 *
 * The raster handed back to the caller holds the canvas *before* disposal,
 * because that is the picture the frame describes; the next frame starts from
 * the canvas *after* it.  The two differ, so the snapshot is a copy that gets
 * disposed of rather than the raster itself.
 *
 * Failing to take the snapshot is not a decode failure - the answer is
 * already computed and correct, and all that is lost is the head start for
 * the next frame.
 */
static void gif_cache_store(gimg_gif_doc_state_t * state,
    const GIMG_Allocator * alloc, size_t index, const uint8_t * canvas,
    size_t stride, size_t canvas_bytes, const uint8_t * saved) {
  // Asked first, because the answer is usually no and the work below is a
  // canvas-sized copy.  A walk in reverse replaces nothing at all, and paying
  // that copy for every frame of it would be most of an animation's worth of
  // memory moved for nothing.  The check is made again below, under the same
  // lock as the write, so this one racing is only ever wasted work.
  GCU_MUTEX_LOCK(state->cache_lock);
  const bool worth_taking =
      !state->cache_canvas || state->cache_next <= index;
  GCU_MUTEX_UNLOCK(state->cache_lock);
  if (!worth_taking) {
    return;
  }

  uint8_t * snapshot = (uint8_t *)gimg_malloc(alloc, canvas_bytes);
  if (!snapshot) {
    return;
  }
  memcpy(snapshot, canvas, canvas_bytes);
  gif_dispose(
      state, &state->frames[index], snapshot, stride, saved, canvas_bytes);

  GCU_MUTEX_LOCK(state->cache_lock);
  // A cache already further along is left where it is.  Two threads walking
  // the same document in opposite directions would otherwise take turns
  // dragging the cache backwards, and neither would ever get a head start.
  // This is the test that counts: another thread may have moved the cache on
  // since the one above.
  if (!state->cache_canvas || state->cache_next <= index) {
    gimg_free(alloc, state->cache_canvas);
    state->cache_canvas = snapshot;
    state->cache_stride = stride;
    state->cache_bytes = canvas_bytes;
    state->cache_next = index + 1u;
    snapshot = NULL;
  }
  GCU_MUTEX_UNLOCK(state->cache_lock);
  gimg_free(alloc, snapshot);
}

/**
 * Forget the cached canvas, so the next decode starts from an empty screen.
 *
 * The cache only ever moves forward (see gif_cache_store), which is what stops
 * two walks in opposite directions from dragging it back and forth.  The cost
 * is that a *second* forward walk over the same document finds the cache
 * parked at the end, where it helps nothing, and replays every frame from the
 * beginning - turning a linear walk quadratic.
 *
 * The GIF writer makes exactly that second walk: it reads every frame once to
 * see whether they share a palette, then again to encode them.  Without this,
 * re-encoding a 358-frame animation took 110 seconds instead of 4.3.  Rather
 * than weaken the forward-only rule, which earns its keep everywhere else, the
 * one caller that knowingly starts over says so.
 */
void gimg_gif_cache_reset(gimg_gif_doc_state_t * state) {
  if (!state || !state->cache_lock_ready) {
    return;
  }
  GCU_MUTEX_LOCK(state->cache_lock);
  uint8_t * stale = state->cache_canvas;
  state->cache_canvas = NULL;
  state->cache_next = 0u;
  state->cache_bytes = 0u;
  state->cache_stride = 0u;
  GCU_MUTEX_UNLOCK(state->cache_lock);
  gimg_free(state->allocator, stale);
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
  size_t canvas_bytes = 0;
  if (!gcu_safe_mul_size(stride, (size_t)state->canvas_height,
          &canvas_bytes)) {
    gimg_raster_destroy(raster);
    return GIMG_ERR_LIMIT;
  }

  // The cache is the one mutable thing in the document state, and decode is
  // handed the document as const.  `codec_private` is a plain `void *`, so
  // reading it back gives a pointer that was never const to begin with - the
  // const is on the caller's view of the document, not on the state - and the
  // mutex is what makes writing through it safe.  A single-image GIF is left
  // out: there is no second frame to give a head start to, so the cache would
  // be a canvas-sized allocation that nothing ever reads.
  gimg_gif_doc_state_t * cache_state =
      (state->frame_count > 1u && state->cache_lock_ready)
      ? (gimg_gif_doc_state_t *)doc->codec_private
      : NULL;

  size_t start = 0;
  bool seeded = false;
  if (cache_state) {
    GCU_MUTEX_LOCK(cache_state->cache_lock);
    if (cache_state->cache_canvas && cache_state->cache_next <= index &&
        cache_state->cache_stride == stride &&
        cache_state->cache_bytes == canvas_bytes) {
      memcpy(canvas, cache_state->cache_canvas, canvas_bytes);
      start = cache_state->cache_next;
      seeded = true;
    }
    GCU_MUTEX_UNLOCK(cache_state->cache_lock);
  }

  if (!seeded) {
    // The logical screen starts empty rather than filled with the background
    // colour.  The specification names a background index (89a 18), but the
    // viewers everyone's files were authored against ignore it and start
    // transparent; filling it would put a colour on screen that no other
    // decoder shows.  The colour that index names is reported through
    // gimg_doc_background_color(), resolved against the Global Color Table, so
    // a caller who does want to honour it can - this codec simply does not
    // decide that on their behalf.
    for (uint32_t y = 0; y < state->canvas_height; y++) {
      memset(canvas + (size_t)y * stride, 0, (size_t)state->canvas_width * 4u);
    }
  }

  uint8_t * saved = NULL;
  unsigned char * indices = NULL;

  for (size_t i = start; i <= index; i++) {
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

    // Only a frame that says it is to be replaced by what was under it needs
    // keeping, so the copy is made here rather than for every frame.  The
    // last frame is copied too, even though its disposal does not apply to
    // the picture being returned, because the cache records the canvas after
    // that disposal.
    if (frame->disposal == GIMG_GIF_DISPOSAL_PREVIOUS) {
      if (!saved) {
        saved = (uint8_t *)gimg_malloc(alloc, canvas_bytes);
        if (!saved) {
          r = GIMG_ERR_OOM;
          break;
        }
      }
      memcpy(saved, canvas, canvas_bytes);
    }

    gif_paint(state, frame, indices, canvas, stride);

    if (i < index) {
      gif_dispose(state, frame, canvas, stride, saved, canvas_bytes);
    }
  }

  if (r == GIMG_OK && cache_state) {
    gif_cache_store(
        cache_state, alloc, index, canvas, stride, canvas_bytes, saved);
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

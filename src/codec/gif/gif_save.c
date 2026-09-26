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
 * GIF save: one image block per frame, each the whole logical screen.
 *
 * BUILDING A PALETTE, AND WHY NOTHING IS QUANTIZED
 * ================================================
 *
 * A GIF pixel is an index into a table of at most 256 colours (89a 18).  An
 * image with more colours than that cannot be written without deciding which
 * to keep, and that decision is quantization - an image-processing choice, not
 * a codec's.  The PNG writer in this library draws the line in the same place
 * and for the same reason: when an image already has 256 colours or fewer
 * there is nothing to choose, because exactly one palette reproduces it, and
 * building that table is a way of storing what is already there.
 *
 * So a raster of more than 256 colours is refused with
 * GIMG_ERR_UNSUPPORTED.  A caller that wants a photograph as a GIF reduces
 * the colours first, with an operation that says what it did.
 *
 * Transparency is the same argument one bit down.  GIF designates a single
 * index transparent and treats every other pixel as opaque; a half-covered
 * edge has nowhere to go.  A partially transparent pixel is therefore refused
 * unless the caller sets `gif_alpha_threshold` and says which way to round.
 *
 * HOW A FRAME IS MADE SMALL
 * =========================
 *
 * A frame handed to this encoder is the whole canvas as it should look; a GIF
 * frame is a patch.  Two things turn one into the other, and both are checked
 * by decoding the result rather than by reasoning about it:
 *
 *   - the frame is cropped to the rectangle in which it differs from what is
 *     already on the screen, and
 *   - inside that rectangle, a pixel that matches what is already on the
 *     screen is written as the transparent index, so the screen shows through
 *     and the code stream compresses into runs of one index.
 *
 * Both need the same thing to be true: the encoder has to know what the screen
 * holds at the moment each frame is drawn.  That is `screen` in
 * gimg_gif_save() - not simply the frame before, because disposal 2 blanks a
 * rectangle after its frame has been shown.  See "Frame optimization" below.
 */

#include <ghoti.io/image/macros.h>
#include <ghoti.io/image/codec.h>
#include <ghoti.io/image/core.h>
#include <ghoti.io/image/doc.h>
#include <ghoti.io/image/meta.h>
#include <ghoti.io/image/raster.h>
#include <ghoti.io/image/stream.h>
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
// Colour lookup
// ---------------------------------------------------------------------------

/** Slots in the colour lookup: twice the largest palette, so it stays sparse. */
#define GIMG_GIF_LUT_SLOTS 512u

/**
 * Open-addressed map from an RGB8 colour to a palette index.
 *
 * The writer needs an index for every pixel and a palette holds up to 256
 * entries, so a linear scan costs 256 comparisons per pixel.  The same shape
 * as the PNG writer's, and for the same reason; a `used` array rather than a
 * sentinel key, because black is a real colour and would make a poor "empty".
 */
typedef struct {
  uint32_t key[GIMG_GIF_LUT_SLOTS];
  uint8_t value[GIMG_GIF_LUT_SLOTS];
  uint8_t used[GIMG_GIF_LUT_SLOTS];
  size_t count;
} gif_color_lut_t;

static void gif_lut_init(gif_color_lut_t * lut) {
  memset(lut->used, 0, sizeof(lut->used));
  lut->count = 0;
}

/** Fibonacci hashing: one multiply, then take the high bits. */
static size_t gif_lut_slot(uint32_t key) {
  return (size_t)((key * UINT32_C(2654435761)) >> 23) &
      (GIMG_GIF_LUT_SLOTS - 1u);
}

static bool gif_lut_get(
    const gif_color_lut_t * lut, uint32_t key, uint8_t * out_value) {
  size_t i = gif_lut_slot(key);
  for (size_t probe = 0; probe < GIMG_GIF_LUT_SLOTS; probe++) {
    if (!lut->used[i]) {
      return false;
    }
    if (lut->key[i] == key) {
      *out_value = lut->value[i];
      return true;
    }
    i = (i + 1u) & (GIMG_GIF_LUT_SLOTS - 1u);
  }
  return false;
}

/** Insert, or leave an existing entry alone.  False when the table is full. */
static bool gif_lut_put(gif_color_lut_t * lut, uint32_t key, uint8_t value,
    size_t limit) {
  size_t i = gif_lut_slot(key);
  for (size_t probe = 0; probe < GIMG_GIF_LUT_SLOTS; probe++) {
    if (!lut->used[i]) {
      if (lut->count >= limit) {
        return false;
      }
      lut->used[i] = 1;
      lut->key[i] = key;
      lut->value[i] = value;
      lut->count++;
      return true;
    }
    if (lut->key[i] == key) {
      return true;
    }
    i = (i + 1u) & (GIMG_GIF_LUT_SLOTS - 1u);
  }
  return false;
}

static uint32_t gif_rgb_key(const unsigned char * p) {
  return ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | (uint32_t)p[2];
}

/** A rectangle of the logical screen: where a frame's image block sits. */
typedef struct {
  uint32_t x;
  uint32_t y;
  uint32_t w;
  uint32_t h;
} gif_rect_t;

/**
 * Whether this pixel is one the encoder will store as transparent.
 *
 * The single place that rule lives.  The planner asks it to decide which index
 * to write, and the frame differ asks it to decide whether two pixels are the
 * same - and those two have to agree, or a frame gets cropped to a rectangle
 * that leaves a real change outside it.
 *
 * `out_refused` is set when the pixel is partly transparent and no threshold
 * was given to round it with; the caller turns that into
 * GIMG_ERR_UNSUPPORTED rather than guessing.
 */
static bool gif_pixel_transparent(
    const uint8_t * px, uint16_t alpha_threshold, bool * out_refused) {
  const uint8_t a = px[3];
  if (a == 0u) {
    return true;
  }
  if (a == 255u) {
    return false;
  }
  if (alpha_threshold == 0u) {
    if (out_refused) {
      *out_refused = true;
    }
    return false;
  }
  return a < alpha_threshold;
}

/**
 * Whether the encoder would store the same thing for both pixels.
 *
 * The equivalence every comparison in this file uses.  Both transparent is the
 * same pixel whatever colour sits under the transparency; otherwise it is the
 * three colour bytes, because alpha above the threshold is not stored at all.
 */
static bool gif_pixel_same(
    const uint8_t * a, const uint8_t * b, uint16_t alpha_threshold) {
  const bool at = gif_pixel_transparent(a, alpha_threshold, NULL);
  const bool bt = gif_pixel_transparent(b, alpha_threshold, NULL);
  if (at || bt) {
    return at && bt;
  }
  return a[0] == b[0] && a[1] == b[1] && a[2] == b[2];
}

// ---------------------------------------------------------------------------
// Frame optimization
// ---------------------------------------------------------------------------
//
// A frame supplied to this encoder is the whole canvas as it should look, but
// a GIF frame is a patch.  Writing every frame at full size is always correct
// and is what this codec used to do; writing only what changed is smaller, and
// is what every other encoder does.
//
// THE SCREEN
// ----------
//
// Everything here is written against one value: what the logical screen holds
// at the moment a frame is drawn.  `screen` in gimg_gif_save() carries it.
// Comparing against the previous frame instead would be almost right and wrong
// where it matters, because disposal 2 blanks a rectangle after its frame has
// been shown, and the frame after that paints onto the hole rather than onto
// its predecessor.
//
// Two pixels count as equal when the encoder would store the same thing for
// both - gif_pixel_same().  Both transparent is equal whatever colour sits
// under the transparency, because that colour is never shown and never
// written.  The screen buffer holds real RGBA, so it can disagree in those
// invisible bytes; every comparison goes through gif_pixel_same(), so it never
// notices.
//
// CROPPING
// --------
//
// If a frame is drawn with disposal 1 - leave it in place - over a screen, and
// its rectangle covers every pixel in which it differs from that screen, then
// after drawing it the screen holds that frame.  By induction the screen is
// always right.
//
// MASKING
// -------
//
// Inside that rectangle, a pixel equal to the screen need not be written at
// all: the transparent index leaves what is already there, which is the pixel
// wanted.  So the planner writes the transparent index for every such pixel.
// It costs one palette entry and buys long runs of a single index, which is
// what the LZW stream is good at.  On a 358-frame animation this is the
// difference between 2.03x the original encoder's size and 1.06x.
//
// Masking is decided for the whole canvas at plan time, before the frame's
// rectangle is known - the rectangle depends on the frame that follows, which
// has not been read yet.  That is harmless: the rectangle is always a subset,
// so a decision made outside it is never written.  It can reserve the
// transparent entry for a frame whose rectangle turns out to contain no masked
// pixel, which costs one of the 256 colours and nothing else.
//
// THE ONE HOLE
// ------------
//
// Painting cannot make an opaque pixel transparent.  GIF has no eraser except
// disposal 2, which blanks exactly the rectangle of the frame carrying it.  So
// where a frame needs a pixel see-through that the screen has opaque, the
// previous frame is grown to cover what must be erased and given disposal 2.
// Writing the whole canvas would also be correct and is what this codec did
// before; on a 358-frame animation with a fading element it cost 89
// full-canvas frames and 94% of the output.
//
// Nothing has to be grown to repaint the hole.  The frame after a disposal 2
// is compared against the screen *after* that disposal, so whatever the hole
// must show is a difference, and cropping picks it up on its own.  What the
// hole should leave transparent is not a difference, and is left alone.
//
// The same test applies to the wrap from the last frame back to the first,
// because a loop makes that a transition like any other, and a first frame
// that is transparent where the last was opaque would otherwise be wrong on
// every pass but the first.

/** The smallest rectangle containing both, for growing one to cover a need. */
static gif_rect_t gif_rect_union(gif_rect_t a, gif_rect_t b) {
  const uint32_t x0 = a.x < b.x ? a.x : b.x;
  const uint32_t y0 = a.y < b.y ? a.y : b.y;
  const uint32_t ax1 = a.x + a.w, bx1 = b.x + b.w;
  const uint32_t ay1 = a.y + a.h, by1 = b.y + b.h;
  const uint32_t x1 = ax1 > bx1 ? ax1 : bx1;
  const uint32_t y1 = ay1 > by1 ? ay1 : by1;
  gif_rect_t out = {x0, y0, x1 - x0, y1 - y0};
  return out;
}

/**
 * Where `cur` needs a pixel transparent that the screen has opaque, as a
 * rectangle.
 *
 * `prev` is the screen as it stands once the frame before `cur` has been
 * painted, which is that frame: disposal has not happened yet, because what
 * this answers is what the disposal has to be.
 *
 * A non-empty answer means the transition cannot be written as a patch alone:
 * painting cannot erase.  The rectangle is what has to be cleared, and clearing
 * only that much is why a fading element does not cost two full-canvas frames
 * every time it fades.
 */
static bool gif_clear_needed(const uint8_t * prev, size_t prev_stride,
    const uint8_t * cur, size_t cur_stride, uint32_t w, uint32_t h,
    uint16_t alpha_threshold, gif_rect_t * out) {
  uint32_t min_x = w, min_y = h, max_x = 0, max_y = 0;
  bool any = false;
  for (uint32_t y = 0; y < h; y++) {
    const uint8_t * p = prev + (size_t)y * prev_stride;
    const uint8_t * c = cur + (size_t)y * cur_stride;
    for (uint32_t x = 0; x < w; x++) {
      if (!gif_pixel_transparent(c + (size_t)x * 4u, alpha_threshold, NULL) ||
          gif_pixel_transparent(p + (size_t)x * 4u, alpha_threshold, NULL)) {
        continue;
      }
      any = true;
      if (x < min_x) {
        min_x = x;
      }
      if (x > max_x) {
        max_x = x;
      }
      if (y < min_y) {
        min_y = y;
      }
      if (y > max_y) {
        max_y = y;
      }
    }
  }
  if (!any) {
    return false;
  }
  out->x = min_x;
  out->y = min_y;
  out->w = max_x - min_x + 1u;
  out->h = max_y - min_y + 1u;
  return true;
}

/**
 * The smallest rectangle covering every pixel where `cur` differs from the
 * screen.
 *
 * `prev` is the screen as it stands when `cur` is about to be painted, after
 * the previous frame's disposal.  That is what makes the disposal 2 case fall
 * out rather than needing a correction: a blanked pixel that must show
 * something differs from the hole and is inside the answer; one that must stay
 * blank does not and is not.
 *
 * A frame identical to what is already on screen has no changed pixels at all,
 * and gets a one-pixel rectangle: GIF has no zero-sized image block, and one
 * pixel repainted its own colour is the cheapest way to say "nothing
 * happened".
 */
static gif_rect_t gif_changed_rect(const uint8_t * prev, size_t prev_stride,
    const uint8_t * cur, size_t cur_stride, uint32_t w, uint32_t h,
    uint16_t alpha_threshold) {
  uint32_t min_x = w, min_y = h, max_x = 0, max_y = 0;
  bool any = false;
  for (uint32_t y = 0; y < h; y++) {
    const uint8_t * p = prev + (size_t)y * prev_stride;
    const uint8_t * c = cur + (size_t)y * cur_stride;
    for (uint32_t x = 0; x < w; x++) {
      if (gif_pixel_same(p + (size_t)x * 4u, c + (size_t)x * 4u,
              alpha_threshold)) {
        continue;
      }
      any = true;
      if (x < min_x) {
        min_x = x;
      }
      if (x > max_x) {
        max_x = x;
      }
      if (y < min_y) {
        min_y = y;
      }
      if (y > max_y) {
        max_y = y;
      }
    }
  }
  gif_rect_t rect;
  if (!any) {
    rect.x = 0u;
    rect.y = 0u;
    rect.w = 1u;
    rect.h = 1u;
    return rect;
  }
  rect.x = min_x;
  rect.y = min_y;
  rect.w = max_x - min_x + 1u;
  rect.h = max_y - min_y + 1u;
  return rect;
}

// ---------------------------------------------------------------------------
// Turning a raster into indices
// ---------------------------------------------------------------------------

/** What one frame turned into: indices, the table they point at, and alpha. */
typedef struct {
  unsigned char * indices;
  gimg_gif_rgb_t palette[GIMG_GIF_MAX_PALETTE];
  uint16_t palette_count;
  bool has_transparency;
  uint8_t transparent_index;
} gif_frame_plan_t;


/**
 * Collect a frame's distinct colours into a palette and index every pixel.
 *
 * `pixels` is the frame as it should look, and `screen` what the logical
 * screen holds where it is about to be drawn - or NULL when nothing is known
 * to be there, which is the first frame of a document.  A pixel equal to the
 * screen is written as the transparent index rather than as its own colour,
 * because leaving the screen showing through produces the pixel wanted and
 * costs one index instead of a colour; see "Frame optimization" above.
 *
 * Transparent pixels - masked or genuine - all take one reserved index, so a
 * frame that uses transparency has 255 colours available rather than 256.
 */
static GIMG_Result gif_plan_frame_once(const uint8_t * pixels, size_t stride,
    uint32_t width, uint32_t height, const uint8_t * screen,
    const GIMG_Allocator * alloc, uint16_t alpha_threshold,
    gif_frame_plan_t * plan) {
  size_t pixel_count = 0;
  if (!gcu_safe_mul_size((size_t)width, (size_t)height, &pixel_count)) {
    return GIMG_ERR_LIMIT;
  }
  plan->indices = (unsigned char *)gimg_malloc(alloc, pixel_count);
  if (!plan->indices) {
    return GIMG_ERR_OOM;
  }
  const size_t screen_stride = (size_t)width * 4u;

  // A first pass settles whether any pixel will be stored as transparent,
  // because that decides which index the colours start at: the transparent one
  // has to be a real entry in the table and cannot also be a colour.
  bool needs_transparent = false;
  for (uint32_t y = 0; y < height && !needs_transparent; y++) {
    const uint8_t * row = pixels + (size_t)y * stride;
    const uint8_t * ref = screen ? screen + (size_t)y * screen_stride : NULL;
    for (uint32_t x = 0; x < width; x++) {
      const uint8_t * px = row + (size_t)x * 4u;
      bool refused = false;
      if (gif_pixel_transparent(px, alpha_threshold, &refused)) {
        needs_transparent = true;
        break;
      }
      if (refused) {
        gimg_free(alloc, plan->indices);
        plan->indices = NULL;
        return GIMG_ERR_UNSUPPORTED;
      }
      if (ref && gif_pixel_same(px, ref + (size_t)x * 4u, alpha_threshold)) {
        needs_transparent = true;
        break;
      }
    }
  }

  gif_color_lut_t lut;
  gif_lut_init(&lut);
  plan->has_transparency = needs_transparent;
  plan->transparent_index = 0u;
  plan->palette_count = 0u;

  // Index 0 is the transparent one when there is one, so that a decoder which
  // ignores the control block shows the background rather than a stray colour.
  // Sixteen bits, not eight: a full table assigns index 255 and then has to
  // count past it to say the table holds 256.  As a uint8_t that increment
  // wrapped to zero, and a 256-colour image was written with a one-entry
  // palette - the only size a GIF can hold that this made unreachable.
  uint16_t next_index = needs_transparent ? 1u : 0u;
  const size_t color_limit =
      needs_transparent ? GIMG_GIF_MAX_PALETTE - 1u : GIMG_GIF_MAX_PALETTE;
  if (needs_transparent) {
    plan->palette[0].r = 0u;
    plan->palette[0].g = 0u;
    plan->palette[0].b = 0u;
  }

  for (uint32_t y = 0; y < height; y++) {
    const uint8_t * row = pixels + (size_t)y * stride;
    const uint8_t * ref = screen ? screen + (size_t)y * screen_stride : NULL;
    for (uint32_t x = 0; x < width; x++) {
      const uint8_t * px = row + (size_t)x * 4u;
      bool refused = false;
      bool transparent = gif_pixel_transparent(px, alpha_threshold, &refused);
      if (refused) {
        gimg_free(alloc, plan->indices);
        plan->indices = NULL;
        return GIMG_ERR_UNSUPPORTED;
      }
      if (!transparent && ref &&
          gif_pixel_same(px, ref + (size_t)x * 4u, alpha_threshold)) {
        transparent = true;
      }
      if (transparent) {
        plan->indices[(size_t)y * width + x] = 0u;
        continue;
      }
      const uint32_t key = gif_rgb_key(px);
      uint8_t index = 0u;
      if (!gif_lut_get(&lut, key, &index)) {
        if (!gif_lut_put(&lut, key, (uint8_t)next_index, color_limit)) {
          // A 257th colour, or a 256th alongside transparency.  Refused rather
          // than quantized; see the file comment.
          gimg_free(alloc, plan->indices);
          plan->indices = NULL;
          return GIMG_ERR_UNSUPPORTED;
        }
        index = (uint8_t)next_index;
        plan->palette[index].r = px[0];
        plan->palette[index].g = px[1];
        plan->palette[index].b = px[2];
        next_index++;
      }
      plan->indices[(size_t)y * width + x] = index;
    }
  }

  plan->palette_count = next_index;
  if (plan->palette_count == 0u) {
    // GIF 89a 18: a table has at least one entry, so a frame that named no
    // colour still needs one.
    //
    // This cannot happen, and the reason is worth writing down because the
    // obvious reading of it is wrong.  "Every pixel was transparent" does not
    // reach here: a transparent pixel is exactly what sets needs_transparent
    // in the pass above, which starts next_index at 1.  Reaching zero would
    // take a frame that named no colour *and* had no transparent pixel, which
    // is a frame with no pixels at all - and width and height are checked
    // before any of this runs.  Measured: a frame whose every pixel is
    // transparent arrives here with a count of one, not zero.
    plan->palette_count = 1u;
  }
  return GIMG_OK;
}

/**
 * gif_plan_frame_once(), and without masking if masking is what did not fit.
 *
 * Masking spends a palette entry on the transparent index.  A frame of exactly
 * 256 colours has none to spare, so a frame that would have been written
 * before masking existed must still be written now: the answer is to write it
 * the long way, with every pixel its own colour, rather than to refuse it.
 *
 * Masking usually *reduces* the colour count, because a masked pixel
 * contributes no colour, so this is reached only by a frame that both fills
 * the table with pixels that changed and has at least one that did not.  The
 * retry costs a second pass over a frame that is about to be refused anyway
 * when the colours are genuinely too many.
 */
static GIMG_Result gif_plan_frame(const uint8_t * pixels, size_t stride,
    uint32_t width, uint32_t height, const uint8_t * screen,
    const GIMG_Allocator * alloc, uint16_t alpha_threshold,
    gif_frame_plan_t * plan) {
  const GIMG_Result r = gif_plan_frame_once(
      pixels, stride, width, height, screen, alloc, alpha_threshold, plan);
  if (r != GIMG_ERR_UNSUPPORTED || !screen) {
    return r;
  }
  return gif_plan_frame_once(
      pixels, stride, width, height, NULL, alloc, alpha_threshold, plan);
}

// ---------------------------------------------------------------------------
// The Global Color Table
// ---------------------------------------------------------------------------
//
// 89a 18 lets one table serve every frame.  This writer used to decline it and
// give each frame a table of its own, on the grounds that two frames of an
// animation rarely share a palette.  Measured on the 19 multi-frame files in
// the test corpus that turned out to be wrong: 17 of them use 255 colours or
// fewer across *every* frame, and their per-frame tables are 2% to 29% of the
// file.  A twelve-frame spinner of sixteen colours was spending 576 bytes of
// 1964 on twelve copies of the same table.
//
// So the frames are walked once before anything is written, to collect the
// colours they use between them.  If they fit one table, that table is written
// once and no frame carries its own; if they do not, nothing is written and
// every frame carries its own exactly as before.
//
// The walk costs one extra decode of each frame - about a quarter again on
// the slowest file in the corpus - and is skipped for a single-frame document,
// where one global table and one local table are the same size and the
// question does not arise.
//
// It is an upper bound rather than the exact answer: it counts the colours in
// the frames as handed over, and masking will only ever remove some.  A bound
// is what is wanted, because a table that is large enough stays large enough.

/** Colours to leave for the frames, after index 0 is reserved transparent. */
#define GIMG_GIF_GLOBAL_MAX (GIMG_GIF_MAX_PALETTE - 1u)

/**
 * Collect every colour every frame uses, if they fit in one table.
 *
 * Index 0 is kept for transparency whether or not any frame turns out to need
 * it.  Nearly every frame after the first does - that is what masking is - and
 * a table cannot be extended once it is written.
 *
 * @return false when they do not fit, when a frame cannot be read, or when a
 *   pixel is partly transparent with no threshold to round it by.  All three
 *   mean "write local tables instead"; the last is refused for real by the
 *   main pass, which reaches the same pixel and reports it.
 */
static bool gif_collect_global_palette(const GIMG_Doc * doc,
    size_t frame_count, uint32_t canvas_w, uint32_t canvas_h,
    uint16_t alpha_threshold, gif_color_lut_t * lut,
    gimg_gif_rgb_t * palette, uint16_t * out_count) {
  gif_lut_init(lut);
  uint16_t next_index = 1u; // 0 is the transparent one.
  palette[0].r = 0u;
  palette[0].g = 0u;
  palette[0].b = 0u;

  for (size_t i = 0; i < frame_count; i++) {
    GIMG_Item * item = gimg_doc_item((GIMG_Doc *)doc, i);
    if (!item) {
      return false;
    }
    GIMG_Raster * raster = gimg_item_raster(item);
    bool owned = false;
    if (!raster) {
      if (gimg_item_decode(item, NULL, &raster) != GIMG_OK || !raster) {
        return false;
      }
      owned = true;
    }
    const GIMG_Pixel_Format * fmt = gimg_raster_format(raster);
    bool ok = fmt && fmt->layout == GIMG_LAYOUT_INTERLEAVED &&
        fmt->channel_count == 4u && fmt->bits_per_channel[0] == 8u &&
        gimg_raster_width(raster) == canvas_w &&
        gimg_raster_height(raster) == canvas_h;
    if (ok) {
      const uint8_t * base =
          (const uint8_t *)gimg_raster_pixels_const(raster);
      const size_t stride = gimg_raster_stride_bytes(raster);
      for (uint32_t y = 0; y < canvas_h && ok; y++) {
        const uint8_t * row = base + (size_t)y * stride;
        for (uint32_t x = 0; x < canvas_w; x++) {
          const uint8_t * px = row + (size_t)x * 4u;
          bool refused = false;
          if (gif_pixel_transparent(px, alpha_threshold, &refused)) {
            continue; // Index 0, already reserved.
          }
          if (refused) {
            ok = false;
            break;
          }
          const uint32_t key = gif_rgb_key(px);
          uint8_t found = 0u;
          if (gif_lut_get(lut, key, &found)) {
            continue;
          }
          if (!gif_lut_put(lut, key, (uint8_t)next_index,
                  GIMG_GIF_GLOBAL_MAX)) {
            ok = false; // More colours between them than one table can hold.
            break;
          }
          palette[next_index].r = px[0];
          palette[next_index].g = px[1];
          palette[next_index].b = px[2];
          next_index++;
        }
      }
    }
    if (owned) {
      gimg_raster_destroy(raster);
    }
    if (!ok) {
      return false;
    }
  }
  *out_count = next_index;
  return true;
}

/**
 * Choose the Background Color Index for the Logical Screen Descriptor (89a
 * 18), adding the colour to the global table when it is not already in it.
 *
 * 89a 18 has no way to leave the field out: a file with a Global Color Table
 * always names one of its entries.  What it can say is that the entry named is
 * the transparent one, which is how an encoder writes "nothing is behind
 * this", and which every frame this writer emits marks transparent because
 * index 0 is the mask index the planner reserves.  So a document that declares
 * no background - or declares one at alpha 0, which is the same statement -
 * gets index 0, and reading that file back reports the colour at alpha 0
 * again.
 *
 * A colour the table does not already hold is appended.  That can push the
 * table up to the next power of two and cost a few bytes, which is the honest
 * price of stating something the file would otherwise not state; a table with
 * no room left cannot state it at all and keeps index 0.
 *
 * @param count In/out: entries in @a palette, grown by one when the colour had
 *              to be added.
 */
static uint8_t gif_background_index(const GIMG_Doc * doc,
    gimg_gif_rgb_t * palette, uint16_t * count) {
  uint8_t rgba[4];
  if (!gimg_doc_background_color(doc, rgba) || rgba[3] == 0u) {
    return 0u;
  }
  // Entry 0 is the transparent one and is not a colour, so the search starts
  // at 1 even when the background happens to be black.
  for (uint16_t i = 1u; i < *count; i++) {
    if (palette[i].r == rgba[0] && palette[i].g == rgba[1] &&
        palette[i].b == rgba[2]) {
      return (uint8_t)i;
    }
  }
  if (*count >= GIMG_GIF_MAX_PALETTE) {
    return 0u;
  }
  const uint16_t at = *count;
  palette[at].r = rgba[0];
  palette[at].g = rgba[1];
  palette[at].b = rgba[2];
  *count = (uint16_t)(at + 1u);
  return (uint8_t)at;
}

/**
 * Index a frame against a table it is already known to fit.
 *
 * The same work gif_plan_frame() does, without the part that builds a palette:
 * every colour is in the global table by construction, because the table was
 * collected from these very frames.  A pixel equal to the screen, or
 * transparent, takes index 0.
 */
static GIMG_Result gif_plan_frame_global(const uint8_t * pixels, size_t stride,
    uint32_t width, uint32_t height, const uint8_t * screen,
    const GIMG_Allocator * alloc, uint16_t alpha_threshold,
    const gif_color_lut_t * lut, gif_frame_plan_t * plan) {
  size_t pixel_count = 0;
  if (!gcu_safe_mul_size((size_t)width, (size_t)height, &pixel_count)) {
    return GIMG_ERR_LIMIT;
  }
  plan->indices = (unsigned char *)gimg_malloc(alloc, pixel_count);
  if (!plan->indices) {
    return GIMG_ERR_OOM;
  }
  const size_t screen_stride = (size_t)width * 4u;
  plan->palette_count = 0u; // The frame carries no table of its own.
  plan->transparent_index = 0u;
  plan->has_transparency = false;

  for (uint32_t y = 0; y < height; y++) {
    const uint8_t * row = pixels + (size_t)y * stride;
    const uint8_t * ref = screen ? screen + (size_t)y * screen_stride : NULL;
    for (uint32_t x = 0; x < width; x++) {
      const uint8_t * px = row + (size_t)x * 4u;
      bool refused = false;
      bool clear = gif_pixel_transparent(px, alpha_threshold, &refused);
      if (refused) {
        gimg_free(alloc, plan->indices);
        plan->indices = NULL;
        return GIMG_ERR_UNSUPPORTED;
      }
      if (!clear && ref &&
          gif_pixel_same(px, ref + (size_t)x * 4u, alpha_threshold)) {
        clear = true;
      }
      if (clear) {
        plan->indices[(size_t)y * width + x] = 0u;
        plan->has_transparency = true;
        continue;
      }
      uint8_t index = 0u;
      if (!gif_lut_get(lut, gif_rgb_key(px), &index)) {
        // Unreachable: the table was collected from these frames.  Refusing
        // beats writing an index that names the wrong colour.
        gimg_free(alloc, plan->indices);
        plan->indices = NULL;
        return GIMG_ERR_INTERNAL;
      }
      plan->indices[(size_t)y * width + x] = index;
    }
  }
  return GIMG_OK;
}

// ---------------------------------------------------------------------------
// Writing
// ---------------------------------------------------------------------------

/**
 * Write a whole buffer, or fail.
 *
 * gimg_stream_write() refuses a NULL out_bytes_written with
 * GIMG_ERR_INTERNAL, and reports a partial write by returning OK with a
 * smaller count.  Neither is something a caller wants to remember at thirty
 * call sites, and a short write that reads as success is how a truncated file
 * gets produced without an error.
 */
static GIMG_Result gif_write(
    GIMG_Stream * stream, const void * data, size_t size) {
  size_t written = 0;
  const GIMG_Result r = gimg_stream_write(stream, data, size, &written);
  if (r != GIMG_OK) {
    return r;
  }
  return written == size ? GIMG_OK : GIMG_ERR_IO;
}

/** Colour-table size field: the exponent e such that the table has 2^(e+1). */
static uint8_t gif_table_bits(uint16_t count) {
  uint8_t bits = 0u;
  while ((1u << (bits + 1u)) < count) {
    bits++;
  }
  return bits;
}

/** The smallest LZW code size that can name every entry (89a 22). */
static uint8_t gif_min_code_size(uint16_t count) {
  uint8_t size = 2u;
  while ((1u << size) < count) {
    size++;
  }
  return size;
}

static GIMG_Result gif_write_u16(GIMG_Stream * stream, uint16_t v) {
  unsigned char b[2] = {(unsigned char)(v & 0xFFu),
      (unsigned char)((v >> 8) & 0xFFu)};
  return gif_write(stream, b, sizeof(b));
}

/** Write a colour table padded to the 2^(bits+1) entries the field implies. */
static GIMG_Result gif_write_table(GIMG_Stream * stream,
    const gimg_gif_rgb_t * palette, uint16_t count, uint8_t bits) {
  const uint32_t entries = 1u << (bits + 1u);
  for (uint32_t i = 0; i < entries; i++) {
    unsigned char rgb[3] = {0, 0, 0};
    if (i < count) {
      rgb[0] = palette[i].r;
      rgb[1] = palette[i].g;
      rgb[2] = palette[i].b;
    }
    GIMG_Result r = gif_write(stream, rgb, sizeof(rgb));
    if (r != GIMG_OK) {
      return r;
    }
  }
  return GIMG_OK;
}

/** Chop a payload into the length-prefixed chain the format uses (89a 15). */
static GIMG_Result gif_write_sub_blocks(
    GIMG_Stream * stream, const unsigned char * data, size_t size) {
  size_t offset = 0;
  while (offset < size) {
    size_t piece = size - offset;
    if (piece > 255u) {
      piece = 255u;
    }
    unsigned char len = (unsigned char)piece;
    GIMG_Result r = gif_write(stream, &len, 1u);
    if (r != GIMG_OK) {
      return r;
    }
    r = gif_write(stream, data + offset, piece);
    if (r != GIMG_OK) {
      return r;
    }
    offset += piece;
  }
  const unsigned char terminator = 0u;
  return gif_write(stream, &terminator, 1u);
}

/**
 * Write one Comment Extension: the introducer, the label, and the text as a
 * sub-block chain (89a 24).
 */
static GIMG_Result gif_write_comment(
    GIMG_Stream * stream, const unsigned char * text, size_t len) {
  const unsigned char head[2] = {0x21u, 0xFEu};
  GIMG_Result r = gif_write(stream, head, sizeof(head));
  if (r != GIMG_OK) {
    return r;
  }
  return gif_write_sub_blocks(stream, text, len);
}

/**
 * Write the document's comments, before the first image (89a 24 places no
 * constraint; every writer puts them here and a reader looking for a
 * file-level comment looks here).
 *
 * The raw block is preferred when present, because it holds every comment the
 * source carried rather than just the one that was normalized.  The common
 * description is the fallback, which is what a caller who built a document by
 * hand - or edited the description of a loaded one - will have set.
 *
 * A caller who edits the description of a document that also carries a raw
 * block is asking two things at once; the raw block wins, because it is the
 * more specific statement.  Dropping the raw block (GIMG_META_KEEP_COMMON_ONLY)
 * is how the caller says the description is the one they mean.
 */
static GIMG_Result gif_write_comments(GIMG_Stream * stream,
    const GIMG_Doc * doc, const GIMG_Save_Options * options,
    const GIMG_Allocator * alloc) {
  const GIMG_Meta_Policy policy =
      options ? options->metadata_policy : GIMG_META_PRESERVE_ALL;
  if (policy == GIMG_META_DROP_ALL) {
    return GIMG_OK;
  }

  if (policy != GIMG_META_KEEP_COMMON_ONLY) {
    GIMG_Meta_Raw * raw = gimg_doc_meta_raw(doc);
    size_t size = 0;
    if (raw &&
        gimg_meta_raw_get(raw, "gif", GIMG_GIF_RAW_COMMENT, NULL, &size) ==
            GIMG_OK &&
        size > 0u) {
      unsigned char * block = (unsigned char *)gimg_malloc(alloc, size);
      if (!block) {
        return GIMG_ERR_OOM;
      }
      GIMG_Result r =
          gimg_meta_raw_get(raw, "gif", GIMG_GIF_RAW_COMMENT, block, &size);
      size_t offset = 0;
      while (r == GIMG_OK &&
          offset + GIMG_GIF_RAW_COMMENT_PREFIX <= size) {
        const size_t len = ((size_t)block[offset] << 24) |
            ((size_t)block[offset + 1] << 16) |
            ((size_t)block[offset + 2] << 8) | (size_t)block[offset + 3];
        offset += GIMG_GIF_RAW_COMMENT_PREFIX;
        if (len > size - offset) {
          // A block whose framing does not add up is not this writer's to
          // guess at; what has been written stays, and the rest is dropped
          // rather than read past the end of the buffer.
          break;
        }
        if (len > 0u) {
          r = gif_write_comment(stream, block + offset, len);
        }
        offset += len;
      }
      gimg_free(alloc, block);
      return r;
    }
  }

  if (policy == GIMG_META_KEEP_RAW_ONLY) {
    return GIMG_OK;
  }
  GIMG_Meta_Common * common = gimg_doc_meta_common(doc);
  const char * desc = common ? gimg_meta_common_description(common) : NULL;
  if (desc && *desc) {
    return gif_write_comment(
        stream, (const unsigned char *)desc, strlen(desc));
  }
  return GIMG_OK;
}

/** Reorder rows into the four-pass interlace order (89a 20). */
static void gif_interlace_rows(const unsigned char * src, unsigned char * dst,
    uint32_t width, uint32_t height) {
  static const struct {
    uint32_t start;
    uint32_t step;
  } passes[4] = {{0u, 8u}, {4u, 8u}, {2u, 4u}, {1u, 2u}};
  size_t out_row = 0;
  for (size_t p = 0; p < 4u; p++) {
    for (uint32_t y = passes[p].start; y < height; y += passes[p].step) {
      memcpy(dst + out_row * width, src + (size_t)y * width, width);
      out_row++;
    }
  }
}

/** Compress one frame's indices with the GIF LZW profile. */
static GIMG_Result gif_compress_indices(const GIMG_Allocator * alloc,
    const unsigned char * indices, size_t count, uint8_t min_code_size,
    unsigned char ** out_data, size_t * out_size) {
  *out_data = NULL;
  *out_size = 0;

  gcomp_options_t * opts = NULL;
  if (gcomp_options_create(&opts) != GCOMP_OK) {
    return GIMG_ERR_OOM;
  }
  if (gcomp_options_set_string(opts, "lzw.format", "gif") != GCOMP_OK ||
      gcomp_options_set_uint64(opts, "lzw.lit_width", min_code_size) !=
          GCOMP_OK) {
    gcomp_options_destroy(opts);
    return GIMG_ERR_INTERNAL;
  }

  size_t bound = 0;
  if (gcomp_encode_bound(NULL, "lzw", opts, count, &bound) != GCOMP_OK) {
    gcomp_options_destroy(opts);
    return GIMG_ERR_INTERNAL;
  }
  unsigned char * buffer = (unsigned char *)gimg_malloc(alloc, bound);
  if (!buffer) {
    gcomp_options_destroy(opts);
    return GIMG_ERR_OOM;
  }
  size_t written = 0;
  const gcomp_status_t gs = gcomp_encode_buffer(gcomp_registry_default(),
      "lzw", opts, indices, count, buffer, bound, &written);
  gcomp_options_destroy(opts);
  if (gs != GCOMP_OK) {
    gimg_free(alloc, buffer);
    return GIMG_ERR_INTERNAL;
  }
  *out_data = buffer;
  *out_size = written;
  return GIMG_OK;
}

/**
 * Write one frame: its Graphic Control Extension, image descriptor, local
 * colour table and LZW data.
 *
 * `plan` covers the whole canvas; `rect` says which part of it to write.  The
 * indices are cropped here rather than by the planner, because which rectangle
 * a frame needs is not known until the frame after it has been looked at.
 *
 * A `plan` whose palette_count is zero is indexed against the Global Color
 * Table: no local table is written, and `global_count` says how wide the
 * indices are.
 */
static GIMG_Result gif_emit_frame(GIMG_Stream * stream,
    const GIMG_Allocator * alloc, const gif_frame_plan_t * plan,
    gif_rect_t rect, uint32_t canvas_w, uint32_t canvas_h,
    unsigned char disposal, uint16_t delay_cs, bool interlace,
    uint16_t global_count, bool mask_background) {
  (void)canvas_h;
  const bool local = plan->palette_count != 0u;
  const uint16_t table_count = local ? plan->palette_count : global_count;
  const uint8_t bits = gif_table_bits(table_count);
  const uint8_t min_code_size = gif_min_code_size(table_count);

  // `mask_background` asks this frame to say that entry 0 - the index the
  // screen descriptor names as the background when the document declares none
  // - is transparent.  89a 18 has no way to leave that field out, so naming a
  // transparent entry is the only way a file can say "nothing is behind this",
  // and without it a reader sees entry 0's colour, which is black, and reports
  // a background nobody declared.
  //
  // It costs no bytes and changes no pixel.  The flag and the index byte are
  // already in the control block every animated frame carries, and a frame
  // that has no masked pixels has no pixel at index 0 to begin with: index 0
  // is the mask index the planner reserves and no colour is ever put there.
  const bool transparent = plan->has_transparency || mask_background;
  const uint8_t transparent_index =
      plan->has_transparency ? plan->transparent_index : 0u;

  // A Graphic Control Extension is written when the frame needs one: to carry
  // a delay, to name the transparent index, or to say how to dispose of it.
  if (transparent || delay_cs != 0u || disposal != 0u) {
    unsigned char gce[8] = {
        0x21u, 0xF9u, 0x04u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u};
    gce[3] = (unsigned char)(((unsigned char)(disposal << 2)) |
        (unsigned char)(transparent ? 0x01u : 0x00u));
    gce[4] = (unsigned char)(delay_cs & 0xFFu);
    gce[5] = (unsigned char)((delay_cs >> 8) & 0xFFu);
    gce[6] = transparent_index;
    GIMG_Result gr = gif_write(stream, gce, sizeof(gce));
    if (gr != GIMG_OK) {
      return gr;
    }
  }

  unsigned char desc = GIMG_GIF_BLOCK_IMAGE;
  GIMG_Result r = gif_write(stream, &desc, 1u);
  if (r == GIMG_OK) {
    r = gif_write_u16(stream, (uint16_t)rect.x);
  }
  if (r == GIMG_OK) {
    r = gif_write_u16(stream, (uint16_t)rect.y);
  }
  if (r == GIMG_OK) {
    r = gif_write_u16(stream, (uint16_t)rect.w);
  }
  if (r == GIMG_OK) {
    r = gif_write_u16(stream, (uint16_t)rect.h);
  }
  if (r == GIMG_OK) {
    const unsigned char packed = (unsigned char)((local ? 0x80u : 0x00u) |
        (interlace ? 0x40u : 0x00u) | (local ? bits : 0u));
    r = gif_write(stream, &packed, 1u);
  }
  if (r == GIMG_OK && local) {
    r = gif_write_table(stream, plan->palette, plan->palette_count, bits);
  }
  if (r != GIMG_OK) {
    return r;
  }

  size_t pixels = 0;
  if (!gcu_safe_mul_size((size_t)rect.w, (size_t)rect.h, &pixels)) {
    return GIMG_ERR_LIMIT;
  }
  unsigned char * rows = (unsigned char *)gimg_malloc(alloc, pixels);
  if (!rows) {
    return GIMG_ERR_OOM;
  }
  for (uint32_t y = 0; y < rect.h; y++) {
    memcpy(rows + (size_t)y * rect.w,
        plan->indices + (size_t)(rect.y + y) * canvas_w + rect.x, rect.w);
  }

  unsigned char * woven = NULL;
  if (interlace) {
    woven = (unsigned char *)gimg_malloc(alloc, pixels);
    if (!woven) {
      gimg_free(alloc, rows);
      return GIMG_ERR_OOM;
    }
    gif_interlace_rows(rows, woven, rect.w, rect.h);
  }

  unsigned char * codes = NULL;
  size_t codes_size = 0;
  r = gif_compress_indices(alloc, woven ? woven : rows, pixels, min_code_size,
      &codes, &codes_size);
  gimg_free(alloc, woven);
  gimg_free(alloc, rows);
  if (r != GIMG_OK) {
    return r;
  }

  r = gif_write(stream, &min_code_size, 1u);
  if (r == GIMG_OK) {
    r = gif_write_sub_blocks(stream, codes, codes_size);
  }
  gimg_free(alloc, codes);
  return r;
}

/**
 * The Pixel Aspect Ratio byte for what the document declares (89a 18).
 *
 * Carried without a save option, which the loop count needed and this does
 * not.  `gif_loop_count`'s zero already means "forever", so it had no spelling
 * for "unset" and could not fall back to the document without changing what an
 * existing caller's zero meant.  Here zero means the same thing on both sides
 * of the conversion - the format calls it "no information given" and the
 * document model calls it "declares nothing" - so reading the document is
 * unambiguous and a caller who wants no ratio written simply declares none.
 *
 * 89a 18 defines the byte as ratio = (N + 15) / 64 for N from 1 to 255, so the
 * only ratios it can hold run from 16/64 to 270/64.  A document declaring
 * anything outside that gets a zero: saying nothing is right, and saying the
 * nearest expressible thing would be a number the caller never asked for.
 */
static unsigned char gif_aspect_byte(const GIMG_Doc * doc) {
  uint32_t num = 0u, den = 0u;
  if (!gimg_doc_pixel_aspect_ratio(doc, &num, &den) || num == 0u ||
      den == 0u) {
    return 0u;
  }
  const uint64_t scaled =
      ((uint64_t)num * 64u + (uint64_t)den / 2u) / (uint64_t)den;
  if (scaled < 16u || scaled > 270u) {
    return 0u;
  }
  return (unsigned char)(scaled - 15u);
}

GIMG_Result gimg_gif_save(GIMG_Codec * codec, const GIMG_Doc * doc,
    GIMG_Stream * stream, const char * format_name,
    const GIMG_Save_Options * options, GIMG_Save_Report * report) {
  (void)format_name;
  if (!codec || !doc || !stream || !report) {
    return GIMG_ERR_INTERNAL;
  }
  const size_t frame_count = gimg_doc_item_count(doc);
  if (frame_count == 0u) {
    return GIMG_ERR_UNSUPPORTED;
  }

  const GIMG_Allocator * alloc = gimg_alloc_or_default(codec->allocator);
  const uint16_t alpha_threshold = options ? options->gif_alpha_threshold : 0u;
  const bool interlace = options && options->gif_interlace;

  // The canvas is the first frame's size.  A document whose frames disagree is
  // refused rather than padded: this writer produces full-canvas frames, so a
  // differing size is a question about placement that the caller has not been
  // asked.
  GIMG_Raster * first = gimg_item_raster(gimg_doc_item((GIMG_Doc *)doc, 0));
  bool first_owned = false;
  if (!first) {
    // The decode's own answer, not a flat UNSUPPORTED. An allocation that
    // failed here is an out-of-memory, and a caller told the image is
    // unsupported has no reason to free something and try again.
    const GIMG_Result dr =
        gimg_item_decode(gimg_doc_item((GIMG_Doc *)doc, 0), NULL, &first);
    if (dr != GIMG_OK || !first) {
      return dr != GIMG_OK ? dr : GIMG_ERR_UNSUPPORTED;
    }
    first_owned = true;
  }
  const uint32_t canvas_w = gimg_raster_width(first);
  const uint32_t canvas_h = gimg_raster_height(first);
  if (first_owned) {
    gimg_raster_destroy(first);
  }
  if (canvas_w == 0u || canvas_h == 0u || canvas_w > 0xFFFFu ||
      canvas_h > 0xFFFFu) {
    return GIMG_ERR_UNSUPPORTED;
  }

  // One table for every frame when the frames fit one, which is the common
  // case for an animation and saves a copy of the table per frame; otherwise
  // none, and each frame carries its own.  89a 18 makes it optional.
  gif_color_lut_t global_lut;
  gimg_gif_rgb_t global_palette[GIMG_GIF_MAX_PALETTE];
  uint16_t global_count = 0u;
  const bool use_global = frame_count > 1u &&
      gif_collect_global_palette(doc, frame_count, canvas_w, canvas_h,
          alpha_threshold, &global_lut, global_palette, &global_count);
  if (frame_count > 1u && doc->loaded_by_codec == codec && doc->codec_private) {
    // The walk above left this document's canvas cache at the last frame it
    // touched, and the encoding pass below starts again at the first.  The
    // cache only moves forward, so left alone it would help nothing and every
    // frame would replay from the beginning: 110 seconds instead of 4.3 on a
    // 358-frame animation.  Nothing else in the library walks a document
    // twice, which is why this is the one place that has to say so.
    gimg_gif_cache_reset((gimg_gif_doc_state_t *)doc->codec_private);
  }

  GIMG_Result r = gif_write(stream, "GIF89a", 6u);
  if (r != GIMG_OK) {
    return r;
  }
  r = gif_write_u16(stream, (uint16_t)canvas_w);
  if (r == GIMG_OK) {
    r = gif_write_u16(stream, (uint16_t)canvas_h);
  }
  if (r != GIMG_OK) {
    return r;
  }
  unsigned char background = 0u;
  {
    // Packed field, background index, aspect ratio.  Both the background and
    // the aspect ratio are whatever the document declares, which for a
    // document loaded from a GIF is what that file declared.
    //
    // The background index has to be chosen before the table size is, because
    // stating a colour the table does not already hold adds an entry and can
    // take the table up to the next power of two.  Without a Global Color
    // Table there is no entry to name and 89a 18 says the field is then to be
    // zero and ignored, so nothing is stated and nothing is read back.
    if (use_global) {
      background = gif_background_index(doc, global_palette, &global_count);
    }
    const uint8_t global_bits = gif_table_bits(global_count);
    unsigned char tail[3] = {0x70u, background, gif_aspect_byte(doc)};
    if (use_global) {
      tail[0] = (unsigned char)(0x80u | 0x70u | global_bits);
    }
    r = gif_write(stream, tail, sizeof(tail));
    if (r == GIMG_OK && use_global) {
      r = gif_write_table(stream, global_palette, global_count, global_bits);
    }
    if (r != GIMG_OK) {
      return r;
    }
  }

  // The NETSCAPE2.0 Application Extension, which is where every decoder looks
  // for a loop count (89a 26 describes the block; the count inside it is a
  // convention rather than part of the specification).
  //
  // Unlike the background colour and the aspect ratio, this block can be left
  // out, and leaving it out is a different instruction from any count: browsers
  // play such a file once.  So a document that declares no count is written
  // with no block, which is what gimg_doc_clear_loop_count() has always said
  // happens and until now did not - the writer put a count of zero in every
  // animation it produced, turning "said nothing" into "repeat forever".
  //
  // It also used to ignore the document entirely, so loading an animation that
  // asked to repeat five times and saving it produced one that repeats for
  // ever.  ImageMagick and Pillow both preserve the count and both preserve
  // its absence; this was the odd one out in both directions.
  //
  // `gif_loop_count` stays an override rather than the source, because its
  // zero already means "repeat forever" and has no spelling for "not set".
  // Non-zero means the caller asked for a count and gets it; zero means they
  // did not ask, and the document answers.  A caller who wants "for ever" on a
  // document that says otherwise says so with gimg_doc_set_loop_count(doc, 0).
  // Frame count does not gate this.  A loop count on a still image has nothing
  // to repeat, but files carry one - gif_4x2_netscape_loop.gif is a real
  // single-frame GIF with the block - and this codec's own loader reads it
  // from them.  Dropping on write what the accessor reports on read is the
  // silent loss this whole arrangement exists to prevent, and Pillow keeps it
  // too.  ImageMagick is the one that drops it, which costs it a round trip.
  bool write_loop = false;
  uint16_t loops = 0u;
  {
    uint32_t declared = 0u;
    if (options && options->gif_loop_count != 0u) {
      write_loop = true;
      loops = options->gif_loop_count;
    }
    else if (gimg_doc_loop_count(doc, &declared)) {
      write_loop = true;
      // 89a 26's count is two bytes.  A document carrying more than that came
      // from a format with a wider field - APNG's num_plays is four - and the
      // nearest thing GIF can say is the largest count it has.
      loops = declared > 0xFFFFu ? (uint16_t)0xFFFFu : (uint16_t)declared;
    }
  }
  if (write_loop) {
    unsigned char ext[14] = {0x21u, 0xFFu, 0x0Bu, 'N', 'E', 'T', 'S', 'C', 'A',
        'P', 'E', '2', '.', '0'};
    r = gif_write(stream, ext, sizeof(ext));
    if (r != GIMG_OK) {
      return r;
    }
    unsigned char body[5] = {0x03u, 0x01u, (unsigned char)(loops & 0xFFu),
        (unsigned char)((loops >> 8) & 0xFFu), 0x00u};
    r = gif_write(stream, body, sizeof(body));
    if (r != GIMG_OK) {
      return r;
    }
  }

  r = gif_write_comments(stream, doc, options, alloc);
  if (r != GIMG_OK) {
    return r;
  }

  // The frames are written one behind: a frame's disposal depends on whether
  // the frame after it needs a cleared screen, so nothing can be emitted until
  // the next one has been looked at.  `held` is the frame planned but not yet
  // written, and `held_rect` the rectangle it would use if no clear is needed.
  //
  // `screen` is what the logical screen holds, and the order inside the loop
  // exists to keep it honest: a frame is emitted, the screen is advanced past
  // it and past its disposal, and only then is the next frame cropped and
  // planned against it.  Doing either of those first would measure the frame
  // against its predecessor instead of against the screen, which differs
  // exactly where disposal 2 has blanked something.
  gif_frame_plan_t held;
  memset(&held, 0, sizeof(held));
  bool holding = false;
  bool first_emit = true;
  gif_rect_t held_rect = {0u, 0u, 0u, 0u};
  uint16_t held_delay = 0u;
  uint8_t * prev_rgba = NULL;
  uint8_t * first_rgba = NULL;
  const size_t canvas_bytes = (size_t)canvas_w * (size_t)canvas_h * 4u;
  const size_t flat = (size_t)canvas_w * 4u;

  uint8_t * screen = (uint8_t *)gimg_malloc(alloc, canvas_bytes);
  if (!screen) {
    return GIMG_ERR_OOM;
  }
  // 89a 18 gives the logical screen a background colour and no decoder in use
  // paints it; an empty screen is what a viewer actually shows.
  memset(screen, 0, canvas_bytes);

  for (size_t i = 0; i <= frame_count; i++) {
    // One extra turn, to flush the frame still held after the last one.
    const bool flushing = i == frame_count;

    uint8_t * cur_rgba = NULL;
    uint16_t delay_cs = 0u;

    if (!flushing) {
      GIMG_Item * item = gimg_doc_item((GIMG_Doc *)doc, i);
      if (!item) {
        r = GIMG_ERR_INTERNAL;
        goto done;
      }
      GIMG_Raster * raster = gimg_item_raster(item);
      bool owned = false;
      if (!raster) {
        // As above: an allocation failure decoding a frame is an OOM, and
        // saying UNSUPPORTED instead tells the caller the picture is at
        // fault.
        const GIMG_Result dr = gimg_item_decode(item, NULL, &raster);
        if (dr != GIMG_OK || !raster) {
          r = dr != GIMG_OK ? dr : GIMG_ERR_UNSUPPORTED;
          goto done;
        }
        owned = true;
      }
      {
        // Every decode in this library hands back RGBA8; anything else
        // reaching here is a raster the caller built, and converting it is not
        // this codec's job.
        const GIMG_Pixel_Format * fmt = gimg_raster_format(raster);
        const bool usable = fmt && fmt->layout == GIMG_LAYOUT_INTERLEAVED &&
            fmt->channel_count == 4u && fmt->bits_per_channel[0] == 8u &&
            gimg_raster_width(raster) == canvas_w &&
            gimg_raster_height(raster) == canvas_h;
        if (!usable) {
          if (owned) {
            gimg_raster_destroy(raster);
          }
          r = GIMG_ERR_UNSUPPORTED;
          goto done;
        }
      }

      // A flat copy of the frame's pixels, kept so the next frame can be
      // compared against it after the raster itself is gone.
      cur_rgba = (uint8_t *)gimg_malloc(alloc, canvas_bytes);
      if (!cur_rgba) {
        if (owned) {
          gimg_raster_destroy(raster);
        }
        r = GIMG_ERR_OOM;
        goto done;
      }
      {
        const uint8_t * base =
            (const uint8_t *)gimg_raster_pixels_const(raster);
        const size_t stride = gimg_raster_stride_bytes(raster);
        for (uint32_t y = 0; y < canvas_h; y++) {
          memcpy(cur_rgba + (size_t)y * flat, base + (size_t)y * stride, flat);
        }
      }
      if (owned) {
        gimg_raster_destroy(raster);
      }

      uint16_t delay_num = 0, delay_den = 0;
      gimg_item_frame_delay(item, &delay_num, &delay_den);
      if (delay_den != 0u) {
        // The item model carries a fraction; GIF counts hundredths (89a 23).
        delay_cs = (uint16_t)(((uint32_t)delay_num * 100u) / delay_den);
      }
    }

    if (holding) {
      // What the held frame has to do for the one that follows it.  On the
      // flushing turn that is the wrap back to frame 0, which a loop makes a
      // transition like any other.  The screen at this point holds the frame
      // before the held one; painting the held one makes it `prev_rgba`, which
      // is what the comparison below is against.
      const uint8_t * next_rgba = flushing ? first_rgba : cur_rgba;
      bool clear_after = false;
      gif_rect_t clear_rect = {0u, 0u, 0u, 0u};
      gif_rect_t emit_rect = held_rect;
      if (next_rgba && frame_count > 1u) {
        clear_after = gif_clear_needed(prev_rgba, flat, next_rgba, flat,
            canvas_w, canvas_h, alpha_threshold, &clear_rect);
        if (clear_after) {
          // Grow this frame to cover what has to be erased, because disposal 2
          // erases exactly this frame's rectangle and nothing else.
          emit_rect = gif_rect_union(held_rect, clear_rect);
        }
      }
      const unsigned char disposal = (unsigned char)(frame_count <= 1u ? 0u
              : clear_after                                            ? 2u
                                                                       : 1u);
      r = gif_emit_frame(stream, alloc, &held, emit_rect, canvas_w, canvas_h,
          disposal, held_delay, interlace, global_count,
          first_emit && use_global && background == 0u);
      first_emit = false;
      gimg_free(alloc, held.indices);
      memset(&held, 0, sizeof(held));
      holding = false;
      if (r != GIMG_OK) {
        gimg_free(alloc, cur_rgba);
        goto done;
      }

      // Advance the screen past the frame just written and past its disposal.
      // The frame is on the screen whole: its rectangle covered everything it
      // changed, and the pixels it left transparent were the ones already
      // right.  Disposal 2 then blanks its rectangle (89a 23).
      memcpy(screen, prev_rgba, canvas_bytes);
      if (clear_after) {
        for (uint32_t y = 0; y < emit_rect.h; y++) {
          memset(screen + (size_t)(emit_rect.y + y) * flat +
                  (size_t)emit_rect.x * 4u,
              0, (size_t)emit_rect.w * 4u);
        }
      }
    }

    if (flushing) {
      gimg_free(alloc, cur_rgba);
      break;
    }

    if (i == 0u) {
      // The first frame is written whole.  Cropping it to the pixels it draws
      // would also be correct - the wrap check below clears whatever the last
      // frame left where this one is transparent - but a first frame covering
      // the logical screen is what every file in the wild does, and it is what
      // decoders that paint the background colour under it need.
      held_rect.x = 0u;
      held_rect.y = 0u;
      held_rect.w = canvas_w;
      held_rect.h = canvas_h;
    }
    else {
      held_rect = gif_changed_rect(
          screen, flat, cur_rgba, flat, canvas_w, canvas_h, alpha_threshold);
    }
    // At i == 0 the screen is empty, so masking against it marks exactly the
    // pixels that are transparent anyway.
    r = use_global
        ? gif_plan_frame_global(cur_rgba, flat, canvas_w, canvas_h, screen,
              alloc, alpha_threshold, &global_lut, &held)
        : gif_plan_frame(cur_rgba, flat, canvas_w, canvas_h, screen, alloc,
              alpha_threshold, &held);
    if (r != GIMG_OK) {
      gimg_free(alloc, cur_rgba);
      goto done;
    }
    held_delay = delay_cs;
    holding = true;
    if (i == 0u) {
      first_rgba = cur_rgba;
      prev_rgba = cur_rgba;
    }
    else {
      if (prev_rgba != first_rgba) {
        gimg_free(alloc, prev_rgba);
      }
      prev_rgba = cur_rgba;
    }
  }

done:
  if (prev_rgba != first_rgba) {
    gimg_free(alloc, prev_rgba);
  }
  gimg_free(alloc, first_rgba);
  gimg_free(alloc, screen);
  gimg_free(alloc, held.indices);
  if (r != GIMG_OK) {
    return r;
  }

  const unsigned char trailer = GIMG_GIF_BLOCK_TRAILER;
  r = gif_write(stream, &trailer, 1u);
  if (r != GIMG_OK) {
    return r;
  }
  report->bytes_written = gimg_stream_tell(stream);
  return GIMG_OK;
}

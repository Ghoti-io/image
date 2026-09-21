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
 * Colour tables and colour reduction: counting an image's colours, choosing a
 * palette for it, and mapping it onto one.
 *
 * WHY THIS IS NOT IN A CODEC
 * ==========================
 *
 * GIF, palette PNG and palette BMP all store an image as indices into a table
 * of at most 256 colours, and all three writers in this library refuse an
 * image with more colours than that.  That refusal is deliberate: with 256
 * colours or fewer there is exactly one palette that reproduces an image, so
 * building it stores what is already there and decides nothing.  With more,
 * something has to be thrown away, and which colours to lose is a judgement
 * about the picture rather than about the file format.
 *
 * So it lives here, in front of the codecs, where a caller invokes it by name
 * and can see in their own code that colours were discarded.  What comes back
 * is a raster in the source's own pixel format that happens to hold few enough
 * colours - not a new indexed type - so every writer accepts it through the
 * path it already had, and none of them had to learn anything.
 *
 * WHAT COUNTS AS A COLOUR
 * =======================
 *
 * All four samples, with one exception: a fully transparent pixel is the same
 * colour as every other fully transparent pixel, whatever lies under it.  The
 * three palette writers already agree on this - what sits under alpha 0 is
 * never shown and never stored - and without it an image with a transparent
 * border would spend its palette on colours nobody can see.
 */

#include <ghoti.io/image/macros.h>
#include <ghoti.io/image/ops.h>
#include <ghoti.io/image/raster.h>
#include <string.h>

#include "../core/alloc_internal.h"
#include "../core/safe_math_internal.h"

// ---------------------------------------------------------------------------
// Pixel access
// ---------------------------------------------------------------------------
//
// Two formats are handled: RGBA8, which is what every decoder in this library
// produces, and GRAY8.  A grayscale pixel is read as (v, v, v, 255) so that
// everything below works in one space; on the way out it contributes its red
// sample, which is the same as its green and blue for any palette built from
// grayscale pixels.  gimg_ops_palette_apply() refuses a grayscale raster and a
// palette that is not grayscale rather than silently keeping a third of each
// colour.

/** How many samples a pixel of this raster has, or 0 if unsupported. */
static uint8_t pal_channels(const GIMG_Raster * raster) {
  const GIMG_Pixel_Format * fmt = gimg_raster_format(raster);
  if (!fmt || fmt->layout != GIMG_LAYOUT_INTERLEAVED ||
      fmt->channel_type != GIMG_CHANNEL_UNORM ||
      fmt->bits_per_channel[0] != 8u) {
    return 0u;
  }
  if (fmt->channel_count == 4u && fmt->channel_model == GIMG_CHANNEL_RGBA) {
    return 4u;
  }
  if (fmt->channel_count == 1u && fmt->channel_model == GIMG_CHANNEL_GRAY) {
    return 1u;
  }
  return 0u;
}

static void pal_read(const uint8_t * px, uint8_t channels, uint8_t out[4]) {
  if (channels == 4u) {
    out[0] = px[0];
    out[1] = px[1];
    out[2] = px[2];
    out[3] = px[3];
    return;
  }
  out[0] = out[1] = out[2] = px[0];
  out[3] = 255u;
}

/**
 * A colour as one integer, so it can be hashed and compared in one operation.
 *
 * Every fully transparent pixel collapses to zero.  No opaque colour can
 * collide with that: alpha is the low byte, and an opaque pixel has it set.
 */
static uint32_t pal_key(const uint8_t c[4]) {
  if (c[3] == 0u) {
    return 0u;
  }
  return ((uint32_t)c[0] << 24) | ((uint32_t)c[1] << 16) |
      ((uint32_t)c[2] << 8) | (uint32_t)c[3];
}

static void pal_unkey(uint32_t key, uint8_t out[4]) {
  out[0] = (uint8_t)(key >> 24);
  out[1] = (uint8_t)(key >> 16);
  out[2] = (uint8_t)(key >> 8);
  out[3] = (uint8_t)key;
}

// ---------------------------------------------------------------------------
// Histogram
// ---------------------------------------------------------------------------
//
// The distinct colours of one or more images, each with the number of pixels
// carrying it, in the order they were first seen.  An open-addressed index
// over a growing array rather than a table of buckets: median cut wants the
// colours as an array it can permute, and first-appearance order is what makes
// the exact-palette case reproducible.
//
// The colours are kept exactly, not binned into a coarser space as quantizers
// often do.  Binning would make the common case wrong: an image of 200 colours
// has one palette that reproduces it, and two of those colours landing in one
// bin would average them and lose an image nothing needed to lose.

/** The colours seen so far, with an open-addressed index over them. */
typedef struct {
  uint32_t * key;   ///< One per distinct colour, in first-appearance order.
  uint32_t * count; ///< Pixels carrying it.
  size_t n;         ///< Distinct colours so far.
  size_t cap;       ///< Entries `key` and `count` have room for.
  uint32_t * slot;  ///< Hash index: 0 is empty, otherwise the colour's n + 1.
  size_t slots;     ///< Always a power of two.
  const GIMG_Allocator * alloc; ///< Where every array above came from.
} pal_hist_t;

static void pal_hist_free(pal_hist_t * h) {
  gimg_free(h->alloc, h->key);
  gimg_free(h->alloc, h->count);
  gimg_free(h->alloc, h->slot);
  h->key = NULL;
  h->count = NULL;
  h->slot = NULL;
  h->n = h->cap = h->slots = 0;
}

static size_t pal_hash(uint32_t key) {
  // Fibonacci hashing: one multiply, then take the high bits, which is where
  // a multiplicative hash puts the mixing.
  return (size_t)(key * UINT32_C(2654435761));
}

static bool pal_hist_rehash(pal_hist_t * h, size_t slots) {
  uint32_t * fresh = (uint32_t *)gimg_calloc(h->alloc, slots, sizeof(uint32_t));
  if (!fresh) {
    return false;
  }
  gimg_free(h->alloc, h->slot);
  h->slot = fresh;
  h->slots = slots;
  for (size_t i = 0; i < h->n; i++) {
    size_t at = pal_hash(h->key[i]) & (slots - 1u);
    while (h->slot[at]) {
      at = (at + 1u) & (slots - 1u);
    }
    h->slot[at] = (uint32_t)(i + 1u);
  }
  return true;
}

static bool pal_hist_grow(pal_hist_t * h) {
  const size_t cap = h->cap ? h->cap * 2u : 1024u;
  uint32_t * k = (uint32_t *)gimg_realloc(h->alloc, h->key, cap * sizeof(*k));
  if (!k) {
    return false;
  }
  h->key = k;
  uint32_t * c = (uint32_t *)gimg_realloc(h->alloc, h->count, cap * sizeof(*c));
  if (!c) {
    return false;
  }
  h->count = c;
  h->cap = cap;
  return true;
}

/** The colour's position in the array, or SIZE_MAX when it is not there. */
static size_t pal_hist_find(const pal_hist_t * h, uint32_t key) {
  if (!h->slots) {
    return SIZE_MAX;
  }
  size_t at = pal_hash(key) & (h->slots - 1u);
  while (h->slot[at]) {
    const size_t i = h->slot[at] - 1u;
    if (h->key[i] == key) {
      return i;
    }
    at = (at + 1u) & (h->slots - 1u);
  }
  return SIZE_MAX;
}

/** Add one pixel of this colour.  False only on allocation failure. */
static bool pal_hist_add(pal_hist_t * h, uint32_t key) {
  if (h->slots && h->n * 4u < h->slots * 3u) {
    size_t at = pal_hash(key) & (h->slots - 1u);
    while (h->slot[at]) {
      const size_t i = h->slot[at] - 1u;
      if (h->key[i] == key) {
        if (h->count[i] != UINT32_MAX) {
          h->count[i]++;
        }
        return true;
      }
      at = (at + 1u) & (h->slots - 1u);
    }
    if (h->n == h->cap && !pal_hist_grow(h)) {
      return false;
    }
    h->key[h->n] = key;
    h->count[h->n] = 1u;
    h->n++;
    h->slot[at] = (uint32_t)h->n;
    return true;
  }
  // Load factor reached, or nothing allocated yet.
  {
    const size_t want = h->slots ? h->slots * 2u : 2048u;
    if (!pal_hist_rehash(h, want)) {
      return false;
    }
  }
  return pal_hist_add(h, key);
}

/**
 * Walk a raster's pixels into the histogram, stopping early once there are
 * more distinct colours than `limit`.
 *
 * @return false on allocation failure only; hitting the limit is a success
 *   that leaves h->n at limit + 1.
 */
static bool pal_hist_scan(pal_hist_t * h, const GIMG_Raster * src,
    uint8_t channels, size_t limit) {
  const uint8_t * base = (const uint8_t *)gimg_raster_pixels_const(src);
  const size_t stride = gimg_raster_stride_bytes(src);
  const uint32_t w = gimg_raster_width(src);
  const uint32_t hgt = gimg_raster_height(src);
  for (uint32_t y = 0; y < hgt; y++) {
    const uint8_t * row = base + (size_t)y * stride;
    for (uint32_t x = 0; x < w; x++) {
      uint8_t c[4];
      pal_read(row + (size_t)x * channels, channels, c);
      if (!pal_hist_add(h, pal_key(c))) {
        return false;
      }
      if (h->n > limit) {
        return true;
      }
    }
  }
  return true;
}

// ---------------------------------------------------------------------------
// Median cut
// ---------------------------------------------------------------------------
//
// Heckbert, "Color Image Quantization for Frame Buffer Display", SIGGRAPH '82.
// Every colour present starts in one box.  Repeatedly take the box worth
// splitting most, cut it at the median of its longest axis, and stop when
// there are as many boxes as entries wanted.  Each box then contributes the
// average of the colours in it, weighted by how many pixels carry each.
//
// Two choices in that sketch are not forced by it, and both were settled by
// measuring against two quantizers that are not ours rather than by argument -
// see documentation/modules/palette.md:
//
//   - which box is "worth splitting most": its pixel count times the extent of
//     its longest axis.  Pixel count alone spends entries on large flat
//     regions that need one colour each; extent alone spends them on a handful
//     of outlying pixels.
//   - where the median is: the colour at which half the box's *pixels* have
//     been passed, not half its colours.  A colour carried by one pixel and
//     one carried by a million should not pull equally.

/** One box: a run of the colour array, with the bounds of what is in it. */
typedef struct {
  size_t lo;         ///< First colour of the run.
  size_t hi;         ///< One past the last.
  uint8_t min[4];    ///< Lowest sample in the box, per channel.
  uint8_t max[4];    ///< Highest sample in the box, per channel.
  uint64_t pixels;   ///< Pixels carrying any colour in it.
} pal_box_t;

/** The colours under consideration, permuted in place as boxes are split. */
typedef struct {
  uint8_t (*c)[4]; ///< The colours themselves.
  uint32_t * w;    ///< Pixels carrying each.
  size_t n;        ///< How many there are.
} pal_colors_t;

static void pal_box_bounds(pal_box_t * box, const pal_colors_t * cols) {
  uint8_t mn[4] = {255u, 255u, 255u, 255u};
  uint8_t mx[4] = {0u, 0u, 0u, 0u};
  uint64_t pixels = 0;
  for (size_t i = box->lo; i < box->hi; i++) {
    for (int ch = 0; ch < 4; ch++) {
      if (cols->c[i][ch] < mn[ch]) {
        mn[ch] = cols->c[i][ch];
      }
      if (cols->c[i][ch] > mx[ch]) {
        mx[ch] = cols->c[i][ch];
      }
    }
    pixels += cols->w[i];
  }
  memcpy(box->min, mn, 4);
  memcpy(box->max, mx, 4);
  box->pixels = pixels;
}

/** The channel this box is most spread over, and by how much. */
static int pal_box_axis(const pal_box_t * box, int * out_extent) {
  int best = 0, best_extent = -1;
  for (int ch = 0; ch < 4; ch++) {
    const int extent = (int)box->max[ch] - (int)box->min[ch];
    if (extent > best_extent) {
      best_extent = extent;
      best = ch;
    }
  }
  if (out_extent) {
    *out_extent = best_extent;
  }
  return best;
}

/**
 * Sort the box's colours by one channel, using the 256 buckets a byte has.
 *
 * A counting sort rather than a comparison sort because the key is a byte:
 * this is linear in the size of the box where a sort would be n log n, and the
 * whole of median cut is a sequence of these.
 */
static void pal_sort_axis(pal_colors_t * cols, const pal_box_t * box, int axis,
    uint8_t (*tmp_c)[4], uint32_t * tmp_w) {
  size_t bucket[257];
  memset(bucket, 0, sizeof(bucket));
  for (size_t i = box->lo; i < box->hi; i++) {
    bucket[(size_t)cols->c[i][axis] + 1u]++;
  }
  for (size_t v = 1; v < 257u; v++) {
    bucket[v] += bucket[v - 1u];
  }
  for (size_t i = box->lo; i < box->hi; i++) {
    const size_t at = bucket[cols->c[i][axis]]++;
    memcpy(tmp_c[at], cols->c[i], 4);
    tmp_w[at] = cols->w[i];
  }
  const size_t n = box->hi - box->lo;
  memcpy(cols->c + box->lo, tmp_c, n * 4u);
  memcpy(cols->w + box->lo, tmp_w, n * sizeof(uint32_t));
}

/**
 * Choose the colours.
 *
 * `cols` is consumed: its entries are permuted, and on return the first
 * `*out_count` boxes describe the partition.  The caller turns those into
 * palette entries.
 */
static GIMG_Result pal_median_cut(pal_colors_t * cols, uint16_t max_colors,
    const GIMG_Allocator * alloc, GIMG_Palette * out) {
  pal_box_t * boxes =
      (pal_box_t *)gimg_malloc(alloc, (size_t)max_colors * sizeof(pal_box_t));
  uint8_t (*tmp_c)[4] = (uint8_t (*)[4])gimg_malloc(alloc, cols->n * 4u);
  uint32_t * tmp_w =
      (uint32_t *)gimg_malloc(alloc, cols->n * sizeof(uint32_t));
  if (!boxes || !tmp_c || !tmp_w) {
    gimg_free(alloc, boxes);
    gimg_free(alloc, tmp_c);
    gimg_free(alloc, tmp_w);
    return GIMG_ERR_OOM;
  }

  size_t used = 1;
  boxes[0].lo = 0;
  boxes[0].hi = cols->n;
  pal_box_bounds(&boxes[0], cols);

  while (used < (size_t)max_colors) {
    // The box worth splitting most: pixels carried, times how far it reaches.
    size_t pick = SIZE_MAX;
    uint64_t best = 0;
    for (size_t i = 0; i < used; i++) {
      if (boxes[i].hi - boxes[i].lo < 2u) {
        continue; // One colour: there is nothing to cut.
      }
      int extent = 0;
      (void)pal_box_axis(&boxes[i], &extent);
      if (extent <= 0) {
        continue;
      }
      const uint64_t score = boxes[i].pixels * (uint64_t)extent;
      if (pick == SIZE_MAX || score > best) {
        best = score;
        pick = i;
      }
    }
    if (pick == SIZE_MAX) {
      break; // Every box holds one colour: the image has fewer than asked for.
    }

    pal_box_t * box = &boxes[pick];
    const int axis = pal_box_axis(box, NULL);
    pal_sort_axis(cols, box, axis, tmp_c, tmp_w);

    // Cut where half the box's pixels have been passed, so that a colour one
    // pixel wide does not weigh as much as one a million pixels wide.
    const uint64_t half = box->pixels / 2u;
    uint64_t seen = 0;
    size_t cut = box->lo;
    for (size_t i = box->lo; i < box->hi - 1u; i++) {
      seen += cols->w[i];
      cut = i + 1u;
      if (seen > half) {
        break;
      }
    }
    if (cut <= box->lo) {
      cut = box->lo + 1u;
    }
    if (cut >= box->hi) {
      cut = box->hi - 1u;
    }

    pal_box_t right;
    right.lo = cut;
    right.hi = box->hi;
    box->hi = cut;
    pal_box_bounds(box, cols);
    pal_box_bounds(&right, cols);
    boxes[used++] = right;
  }

  for (size_t i = 0; i < used; i++) {
    // The average of the box, weighted by how many pixels carry each colour.
    uint64_t sum[4] = {0, 0, 0, 0};
    uint64_t weight = 0;
    for (size_t j = boxes[i].lo; j < boxes[i].hi; j++) {
      for (int ch = 0; ch < 4; ch++) {
        sum[ch] += (uint64_t)cols->c[j][ch] * cols->w[j];
      }
      weight += cols->w[j];
    }
    for (int ch = 0; ch < 4; ch++) {
      out->entries[i][ch] =
          weight ? (uint8_t)((sum[ch] + weight / 2u) / weight) : 0u;
    }
  }
  out->count = (uint16_t)used;

  gimg_free(alloc, boxes);
  gimg_free(alloc, tmp_c);
  gimg_free(alloc, tmp_w);
  return GIMG_OK;
}

// ---------------------------------------------------------------------------
// Matching
// ---------------------------------------------------------------------------

/** Squared distance over all four samples; alpha counts like a colour. */
static uint32_t pal_dist2(const uint8_t a[4], const uint8_t b[4]) {
  int32_t total = 0;
  for (int ch = 0; ch < 4; ch++) {
    const int32_t d = (int32_t)a[ch] - (int32_t)b[ch];
    total += d * d;
  }
  return (uint32_t)total;
}

static uint16_t pal_nearest(const GIMG_Palette * palette, const uint8_t c[4]) {
  uint16_t best = 0;
  uint32_t best_d = UINT32_MAX;
  for (uint16_t i = 0; i < palette->count; i++) {
    const uint32_t d = pal_dist2(palette->entries[i], c);
    if (d < best_d) {
      best_d = d;
      best = i;
      if (d == 0u) {
        break;
      }
    }
  }
  return best;
}

static bool pal_is_gray(const GIMG_Palette * palette) {
  for (uint16_t i = 0; i < palette->count; i++) {
    const uint8_t * e = palette->entries[i];
    if (e[0] != e[1] || e[1] != e[2] || e[3] != 255u) {
      return false;
    }
  }
  return true;
}

static void pal_write(uint8_t * px, uint8_t channels, const uint8_t c[4]) {
  if (channels == 4u) {
    px[0] = c[0];
    px[1] = c[1];
    px[2] = c[2];
    px[3] = c[3];
    return;
  }
  px[0] = c[0];
}

/** A new raster the same shape as @a src, ready to be filled. */
static GIMG_Result pal_make_output(
    const GIMG_Raster * src, GIMG_Raster ** out) {
  const GIMG_Result r = gimg_raster_create_with_allocator(
      gimg_raster_allocator(src), gimg_raster_width(src),
      gimg_raster_height(src), gimg_raster_format(src), GIMG_RASTER_OWNED,
      NULL, 0, out);
  if (r != GIMG_OK) {
    return r;
  }
  // Rounding a sample to a nearby one does not change what the samples mean.
  const GIMG_Color_Info * info = gimg_raster_color_info_const(src);
  if (info) {
    (void)gimg_raster_set_color_info(*out, info);
  }
  return GIMG_OK;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

static uint16_t pal_max_colors(const GIMG_Quantize_Options * options) {
  const uint16_t want = options ? options->max_colors : 0u;
  if (want == 0u || want > GIMG_PALETTE_MAX_ENTRIES) {
    return (uint16_t)GIMG_PALETTE_MAX_ENTRIES;
  }
  return want;
}

GIMG_Result gimg_ops_count_colors(const GIMG_Raster * src, size_t limit,
    size_t * out_count, bool * out_exact) {
  if (!src || !out_count) {
    return GIMG_ERR_INTERNAL;
  }
  const uint8_t channels = pal_channels(src);
  if (!channels) {
    return GIMG_ERR_UNSUPPORTED;
  }
  const size_t stop = limit ? limit : SIZE_MAX;
  pal_hist_t hist;
  memset(&hist, 0, sizeof(hist));
  hist.alloc = gimg_raster_allocator(src);
  if (!pal_hist_scan(&hist, src, channels, stop)) {
    pal_hist_free(&hist);
    return GIMG_ERR_OOM;
  }
  const bool exact = hist.n <= stop;
  *out_count = exact ? hist.n : stop;
  if (out_exact) {
    *out_exact = exact;
  }
  pal_hist_free(&hist);
  return GIMG_OK;
}

GIMG_Result gimg_ops_palette_from_raster(
    const GIMG_Raster * src, uint16_t max_colors, GIMG_Palette * out_palette) {
  if (!src || !out_palette) {
    return GIMG_ERR_INTERNAL;
  }
  const uint8_t channels = pal_channels(src);
  if (!channels) {
    return GIMG_ERR_UNSUPPORTED;
  }
  if (max_colors == 0u || max_colors > GIMG_PALETTE_MAX_ENTRIES) {
    max_colors = (uint16_t)GIMG_PALETTE_MAX_ENTRIES;
  }
  pal_hist_t hist;
  memset(&hist, 0, sizeof(hist));
  hist.alloc = gimg_raster_allocator(src);
  if (!pal_hist_scan(&hist, src, channels, max_colors)) {
    pal_hist_free(&hist);
    return GIMG_ERR_OOM;
  }
  if (hist.n > max_colors) {
    pal_hist_free(&hist);
    return GIMG_ERR_UNSUPPORTED; // Nothing to build: something must be chosen.
  }
  memset(out_palette, 0, sizeof(*out_palette));
  for (size_t i = 0; i < hist.n; i++) {
    pal_unkey(hist.key[i], out_palette->entries[i]);
  }
  // An image of no pixels has no colours; a table still needs one entry.
  out_palette->count = (uint16_t)(hist.n ? hist.n : 1u);
  pal_hist_free(&hist);
  return GIMG_OK;
}

GIMG_Result gimg_ops_palette_build(const GIMG_Raster * const * src,
    size_t count, const GIMG_Quantize_Options * options,
    GIMG_Palette * out_palette) {
  if (!src || count == 0u || !out_palette) {
    return GIMG_ERR_INTERNAL;
  }
  if (options && (options->method < 0 ||
                     options->method >= GIMG_QUANTIZE_METHOD_COUNT)) {
    return GIMG_ERR_INTERNAL;
  }
  const uint16_t max_colors = pal_max_colors(options);

  uint8_t channels = 0;
  for (size_t i = 0; i < count; i++) {
    if (!src[i]) {
      return GIMG_ERR_INTERNAL;
    }
    const uint8_t ch = pal_channels(src[i]);
    if (!ch || (channels && ch != channels)) {
      // Rasters that disagree about their format have no common colour space
      // to be reduced in; converting one of them is not this function's call.
      return GIMG_ERR_UNSUPPORTED;
    }
    channels = ch;
  }

  pal_hist_t hist;
  memset(&hist, 0, sizeof(hist));
  hist.alloc = gimg_raster_allocator(src[0]);
  for (size_t i = 0; i < count; i++) {
    if (!pal_hist_scan(&hist, src[i], channels, SIZE_MAX)) {
      pal_hist_free(&hist);
      return GIMG_ERR_OOM;
    }
  }

  memset(out_palette, 0, sizeof(*out_palette));
  if (hist.n == 0u) {
    out_palette->count = 1u;
    pal_hist_free(&hist);
    return GIMG_OK;
  }
  if (hist.n <= max_colors) {
    // Few enough already: these are the colours, exactly, in the order they
    // first appeared.  Nothing is averaged and nothing is lost.
    for (size_t i = 0; i < hist.n; i++) {
      pal_unkey(hist.key[i], out_palette->entries[i]);
    }
    out_palette->count = (uint16_t)hist.n;
    pal_hist_free(&hist);
    return GIMG_OK;
  }

  pal_colors_t cols;
  cols.n = hist.n;
  cols.c = (uint8_t (*)[4])gimg_malloc(hist.alloc, cols.n * 4u);
  cols.w = (uint32_t *)gimg_malloc(hist.alloc, cols.n * sizeof(uint32_t));
  if (!cols.c || !cols.w) {
    gimg_free(hist.alloc, cols.c);
    gimg_free(hist.alloc, cols.w);
    pal_hist_free(&hist);
    return GIMG_ERR_OOM;
  }
  for (size_t i = 0; i < cols.n; i++) {
    pal_unkey(hist.key[i], cols.c[i]);
    cols.w[i] = hist.count[i];
  }
  const GIMG_Allocator * alloc = hist.alloc;
  pal_hist_free(&hist);

  const GIMG_Result r = pal_median_cut(&cols, max_colors, alloc, out_palette);
  gimg_free(alloc, cols.c);
  gimg_free(alloc, cols.w);
  return r;
}

/** Map every pixel to its nearest entry, one lookup per distinct colour. */
static GIMG_Result pal_apply_plain(const GIMG_Raster * src, uint8_t channels,
    const GIMG_Palette * palette, GIMG_Raster * dst) {
  const uint8_t * sbase = (const uint8_t *)gimg_raster_pixels_const(src);
  const size_t sstride = gimg_raster_stride_bytes(src);
  uint8_t * dbase = (uint8_t *)gimg_raster_pixels(dst);
  const size_t dstride = gimg_raster_stride_bytes(dst);
  const uint32_t w = gimg_raster_width(src);
  const uint32_t h = gimg_raster_height(src);

  // The nearest entry depends only on the colour, so it is found once per
  // distinct colour rather than once per pixel.  On a photograph that is a few
  // hundred thousand searches instead of a few million; on a screenshot, a few
  // hundred.
  pal_hist_t seen;
  memset(&seen, 0, sizeof(seen));
  seen.alloc = gimg_raster_allocator(src);
  uint16_t * answer = NULL;
  size_t answer_cap = 0;

  for (uint32_t y = 0; y < h; y++) {
    const uint8_t * srow = sbase + (size_t)y * sstride;
    uint8_t * drow = dbase + (size_t)y * dstride;
    for (uint32_t x = 0; x < w; x++) {
      uint8_t c[4];
      pal_read(srow + (size_t)x * channels, channels, c);
      const uint32_t key = pal_key(c);
      size_t at = pal_hist_find(&seen, key);
      if (at == SIZE_MAX) {
        if (!pal_hist_add(&seen, key)) {
          gimg_free(seen.alloc, answer);
          pal_hist_free(&seen);
          return GIMG_ERR_OOM;
        }
        at = seen.n - 1u;
        if (at >= answer_cap) {
          const size_t cap = seen.cap;
          uint16_t * grown = (uint16_t *)gimg_realloc(
              seen.alloc, answer, cap * sizeof(uint16_t));
          if (!grown) {
            gimg_free(seen.alloc, answer);
            pal_hist_free(&seen);
            return GIMG_ERR_OOM;
          }
          answer = grown;
          answer_cap = cap;
        }
        uint8_t canonical[4];
        pal_unkey(key, canonical);
        answer[at] = pal_nearest(palette, canonical);
      }
      pal_write(drow + (size_t)x * channels, channels,
          palette->entries[answer[at]]);
    }
  }
  gimg_free(seen.alloc, answer);
  pal_hist_free(&seen);
  return GIMG_OK;
}

/**
 * Map every pixel to its nearest entry, spreading what the rounding lost into
 * the neighbours not yet visited (Floyd and Steinberg, 1976).
 *
 * Two rows of error are held rather than the whole image, because the weights
 * only ever reach one row ahead.  The direction alternates - a serpentine scan
 * - so that the error does not march to one side and leave a visible drift
 * down that edge.
 *
 * A fully transparent pixel takes neither error nor gives any: it has no
 * colour to be wrong about, and diffusing into it would put the colour of its
 * neighbour underneath something invisible, which the next operation to touch
 * the image would find and use.
 */
static GIMG_Result pal_apply_dither(const GIMG_Raster * src, uint8_t channels,
    const GIMG_Palette * palette, GIMG_Raster * dst) {
  const uint8_t * sbase = (const uint8_t *)gimg_raster_pixels_const(src);
  const size_t sstride = gimg_raster_stride_bytes(src);
  uint8_t * dbase = (uint8_t *)gimg_raster_pixels(dst);
  const size_t dstride = gimg_raster_stride_bytes(dst);
  const uint32_t w = gimg_raster_width(src);
  const uint32_t h = gimg_raster_height(src);
  const GIMG_Allocator * alloc = gimg_raster_allocator(src);

  size_t row_terms = 0;
  if (!gcu_safe_mul_size((size_t)w, 4u, &row_terms)) {
    return GIMG_ERR_LIMIT;
  }
  int32_t * err = (int32_t *)gimg_calloc(alloc, row_terms * 2u,
      sizeof(int32_t));
  if (!err) {
    return GIMG_ERR_OOM;
  }
  int32_t * cur = err;
  int32_t * next = err + row_terms;

  for (uint32_t y = 0; y < h; y++) {
    const uint8_t * srow = sbase + (size_t)y * sstride;
    uint8_t * drow = dbase + (size_t)y * dstride;
    const bool leftward = (y & 1u) != 0u;
    for (uint32_t step = 0; step < w; step++) {
      const uint32_t x = leftward ? (w - 1u - step) : step;
      uint8_t c[4];
      pal_read(srow + (size_t)x * channels, channels, c);

      if (c[3] == 0u) {
        // Nothing to be wrong about, and nothing to hand on.
        const uint16_t idx = pal_nearest(palette, c);
        pal_write(drow + (size_t)x * channels, channels,
            palette->entries[idx]);
        continue;
      }

      uint8_t wanted[4];
      int32_t diff[4];
      for (int ch = 0; ch < 4; ch++) {
        int32_t v = (int32_t)c[ch] + cur[(size_t)x * 4u + (size_t)ch];
        if (v < 0) {
          v = 0;
        }
        if (v > 255) {
          v = 255;
        }
        wanted[ch] = (uint8_t)v;
      }
      const uint16_t idx = pal_nearest(palette, wanted);
      const uint8_t * chosen = palette->entries[idx];
      pal_write(drow + (size_t)x * channels, channels, chosen);
      for (int ch = 0; ch < 4; ch++) {
        diff[ch] = (int32_t)wanted[ch] - (int32_t)chosen[ch];
      }

      // 7/16 ahead, then 3/16, 5/16 and 1/16 into the row below, mirrored when
      // the scan runs the other way.
      const int32_t ahead = leftward ? -1 : 1;
      for (int ch = 0; ch < 4; ch++) {
        const int32_t d = diff[ch];
        if (d == 0) {
          continue;
        }
        const int64_t xa = (int64_t)x + ahead;
        if (xa >= 0 && xa < (int64_t)w) {
          cur[(size_t)xa * 4u + (size_t)ch] += (d * 7) / 16;
        }
        if (y + 1u < h) {
          const int64_t xb = (int64_t)x - ahead;
          if (xb >= 0 && xb < (int64_t)w) {
            next[(size_t)xb * 4u + (size_t)ch] += (d * 3) / 16;
          }
          next[(size_t)x * 4u + (size_t)ch] += (d * 5) / 16;
          if (xa >= 0 && xa < (int64_t)w) {
            next[(size_t)xa * 4u + (size_t)ch] += (d * 1) / 16;
          }
        }
      }
    }
    int32_t * swap = cur;
    cur = next;
    next = swap;
    memset(next, 0, row_terms * sizeof(int32_t));
  }
  gimg_free(alloc, err);
  return GIMG_OK;
}

GIMG_Result gimg_ops_palette_apply(const GIMG_Raster * src,
    const GIMG_Palette * palette, GIMG_Dither dither,
    GIMG_Raster ** out_raster) {
  if (!src || !palette || !out_raster) {
    return GIMG_ERR_INTERNAL;
  }
  *out_raster = NULL;
  if (palette->count == 0u || palette->count > GIMG_PALETTE_MAX_ENTRIES) {
    return GIMG_ERR_INTERNAL;
  }
  if (dither < 0 || dither >= GIMG_DITHER_COUNT) {
    return GIMG_ERR_INTERNAL;
  }
  const uint8_t channels = pal_channels(src);
  if (!channels) {
    return GIMG_ERR_UNSUPPORTED;
  }
  if (channels == 1u && !pal_is_gray(palette)) {
    // A grayscale raster has one sample to store the answer in.  Rather than
    // keep a third of each colour and call it the result, say so.
    return GIMG_ERR_UNSUPPORTED;
  }

  GIMG_Raster * dst = NULL;
  GIMG_Result r = pal_make_output(src, &dst);
  if (r != GIMG_OK) {
    return r;
  }
  r = dither == GIMG_DITHER_FLOYD_STEINBERG
      ? pal_apply_dither(src, channels, palette, dst)
      : pal_apply_plain(src, channels, palette, dst);
  if (r != GIMG_OK) {
    gimg_raster_destroy(dst);
    return r;
  }
  *out_raster = dst;
  return GIMG_OK;
}

GIMG_Result gimg_ops_quantize(const GIMG_Raster * src,
    const GIMG_Quantize_Options * options, GIMG_Raster ** out_raster,
    GIMG_Palette * out_palette) {
  if (!src || !out_raster) {
    return GIMG_ERR_INTERNAL;
  }
  *out_raster = NULL;
  GIMG_Palette palette;
  GIMG_Result r = gimg_ops_palette_build(&src, 1u, options, &palette);
  if (r != GIMG_OK) {
    return r;
  }
  r = gimg_ops_palette_apply(src, &palette,
      options ? options->dither : GIMG_DITHER_NONE, out_raster);
  if (r != GIMG_OK) {
    return r;
  }
  if (out_palette) {
    *out_palette = palette;
  }
  return GIMG_OK;
}

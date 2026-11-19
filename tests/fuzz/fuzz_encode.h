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
 * The encode side of the fuzz harnesses, in one place.
 *
 * Every encode harness used to write the default file, or one option's worth
 * of variation: the PNG, JPEG, BMP, ICO and GIF harnesses saved with an
 * essentially zeroed GIMG_Save_Options, and WebP saved at effort 0 (a present
 * options struct with the field left zero means effort 0).  So the WebP search
 * levels, the PNG row filters and palette policies, JPEG's progressive,
 * arithmetic, lossless, hierarchical, non-interleaved and abbreviated writers
 * and their restart intervals, BMP's run-length and OS/2 encodings, ICO's two
 * payload kinds and TIFF's strip layout were either never run under a fuzzer
 * or reached only when a mutation happened to land on one byte.
 *
 * Here the options are a function of a 32-bit selector, and the selector is a
 * function of the input's own bytes (fuzz_option_selector), the way
 * fuzz_save_policy picks a metadata policy: nothing is consumed from the
 * front, so every seed stays a valid file and the fuzzer moves between option
 * sets by mutating content it already has.  The corpus replay in
 * tests/unit/test_fuzz_corpus.cpp calls the same function with a fixed list of
 * selectors instead, which is what puts the higher-effort writers under
 * `make test` deterministically.
 *
 * What a unit checks, beyond not crashing:
 *
 *   - where the writer is lossless for the options it was given, the decoded
 *     picture comes back equal to the one that was saved (compared only when
 *     both decode to the same pixel format and size, since a writer may
 *     legitimately convert);
 *   - where it is lossy, the file it wrote decodes.
 *
 * A failed check calls fuzz_failure_hook(), which traps by default so that a
 * fuzzer records the input, and which the corpus test replaces with a gtest
 * failure.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#ifndef GIMG_TESTS_FUZZ_FUZZ_ENCODE_H
#define GIMG_TESTS_FUZZ_FUZZ_ENCODE_H

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <set>
#include <string_view>
#include <ghoti.io/image/codec.h>
#include <ghoti.io/image/doc.h>
#include <ghoti.io/image/raster.h>
#include <ghoti.io/image/stream.h>

#include "fuzz_limits.h"

/** Called with a description when a round-trip check fails. */
typedef void (*FuzzFailureFn)(const char * message);

static inline FuzzFailureFn & fuzz_failure_hook() {
  static FuzzFailureFn fn = nullptr;
  return fn;
}

static inline void fuzz_fail(const char * message) {
  if (fuzz_failure_hook() != nullptr) {
    fuzz_failure_hook()(message);
    return;
  }
  fprintf(stderr, "fuzz encode check failed: %s\n", message);
  __builtin_trap();
}

/** What a run of units did, so a caller can say the options were reached. */
struct FuzzEncodeStats {
  size_t saves = 0;       ///< gimg_doc_save calls.
  size_t saves_ok = 0;    ///< ... that returned GIMG_OK.
  size_t compared = 0;    ///< Saves whose picture was compared with the source.
  size_t reloaded = 0;    ///< Saves whose output loaded and decoded.
  uint64_t digest = 0;    ///< Sum of a hash of every output: differs across options.
  size_t distinct_sizes = 0; ///< Unused by the units; for the caller's use.
};

/** The selector for an input: a hash of its head, tail and length. */
static inline uint32_t fuzz_option_selector(const uint8_t * data, size_t size) {
  uint32_t h = 0x9E3779B9u;
  for (size_t i = 0; i < size && i < 48u; i++) {
    h = (h ^ data[i]) * 16777619u;
  }
  for (size_t i = (size > 16u ? size - 16u : 0u); i < size; i++) {
    h = (h ^ data[i]) * 16777619u;
  }
  h = (h ^ (uint32_t)size) * 16777619u;
  h ^= h >> 15;
  return h;
}

/** A small deterministic generator; the options of one selector are one
 * stream of it, so the order the writers draw in does not matter to others. */
struct FuzzRng {
  uint32_t s;
  explicit FuzzRng(uint32_t seed) : s(seed * 2654435761u + 0x7F4A7C15u) {
    next();
    next();
  }
  uint32_t next() {
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return s;
  }
  /** 0 .. n-1. */
  uint32_t below(uint32_t n) { return (next() >> 8) % n; }
  bool one_in(uint32_t n) { return below(n) == 0u; }
};

/** Options plus the arrays they point at, so a spec is copyable by value. */
struct FuzzSaveSpec {
  GIMG_Save_Options opts = {};
  GIMG_JPEG_Progressive_Config progressive = {0, nullptr};
  GIMG_JPEG_Progressive_Scan scans[16] = {};
  /** The writer is lossless for these options: compare pixels. */
  bool exact = false;
  /** Under an exact save, RGB under fully transparent pixels may be cleared. */
  bool alpha0_free = false;
  /** The file is not a complete image (abbreviated table-only stream). */
  bool tables_only = false;
  /** The output cannot be loaded alone (abbreviated image data). */
  bool needs_tables = false;
};

namespace fuzz_detail {

static const GIMG_Meta_Policy kPolicies[] = {GIMG_META_PRESERVE_ALL,
    GIMG_META_DROP_ALL, GIMG_META_STRIP_GPS, GIMG_META_NORMALIZE_EXIF,
    GIMG_META_KEEP_RAW_ONLY, GIMG_META_KEEP_COMMON_ONLY};

static inline void exif_thumb(FuzzRng & r, GIMG_Save_Options & o) {
  static const uint8_t fmts[] = {0, 1, 6, 7};
  o.exif_thumbnail_format = fmts[r.below(4)];
  o.exif_thumbnail_quality = r.one_in(3) ? 0 : (uint8_t)(1 + r.below(100));
}

static inline void fill_jpeg(FuzzRng & r, FuzzSaveSpec & sp) {
  GIMG_Save_Options & o = sp.opts;
  exif_thumb(r, o);
  o.jpeg_quality = r.one_in(4) ? 0u : 1u + r.below(100);
  if (r.one_in(8)) {
    o.jpeg_quality = (r.one_in(2)) ? 1u : 100u;
  }
  o.jpeg_chroma_subsampling = (uint8_t)r.below(3);
  o.jpeg_fdct_method = r.one_in(24) ? GIMG_JPEG_FDCT_REF : GIMG_JPEG_FDCT_LOEFFLER;
  o.jpeg_quant_method = (uint8_t)r.below(2);
  static const uint16_t restarts[] = {0, 0, 0, 1, 2, 3, 5, 16, 100, 65535};
  o.jpeg_restart_interval = restarts[r.below(10)];
  uint32_t p = r.below(32);
  o.jpeg_precision = p == 0 ? 12 : p == 1 ? 8 : p == 2 ? 16 : 0;
  o.jpeg_cmyk_transform = r.one_in(3) ? 2 : (r.one_in(40) ? 1 : 0);
  const bool arith = r.one_in(3);

  const uint32_t mode = r.below(13);
  switch (mode) {
    case 0: // baseline Huffman, the default
    case 1:
      break;
    case 2: // progressive, default script
      o.jpeg_progressive = 1;
      o.jpeg_arithmetic = arith;
      break;
    case 3: { // progressive, a script of the caller's
      o.jpeg_progressive = 1;
      o.jpeg_arithmetic = arith;
      static const GIMG_JPEG_Progressive_Scan kStd[] = {{0, 0, 0, 1}, {1, 5, 0, 2},
          {6, 63, 0, 2}, {1, 63, 2, 1}, {0, 0, 1, 0}, {1, 63, 1, 0}};
      static const GIMG_JPEG_Progressive_Scan kSpectral[] = {
          {0, 0, 0, 0}, {1, 63, 0, 0}};
      static const GIMG_JPEG_Progressive_Scan kBad[] = {{0, 0, 0, 0}, {5, 2, 0, 0}};
      const uint32_t which = r.below(8);
      const GIMG_JPEG_Progressive_Scan * src = which < 4 ? kStd
          : which < 7 ? kSpectral : kBad;
      const unsigned n = which < 4 ? 6u : 2u;
      for (unsigned i = 0; i < n; i++) {
        sp.scans[i] = src[i];
      }
      sp.progressive.scan_count = n;
      sp.progressive.scans = sp.scans;
      o.jpeg_progressive_config = &sp.progressive;
      break;
    }
    case 4: // sequential arithmetic with conditioning
      o.jpeg_arithmetic = 1;
      if (!r.one_in(3)) {
        const uint8_t l = (uint8_t)r.below(16);
        o.jpeg_arith_dc_l = l;
        o.jpeg_arith_dc_u = (uint8_t)(l + r.below(16u - l));
        o.jpeg_arith_ac_k = (uint8_t)r.below(64);
        if (r.one_in(10)) {
          o.jpeg_arith_dc_l = 15;
          o.jpeg_arith_dc_u = 2; // L > U, refused
        }
      }
      break;
    case 5:
    case 6: // lossless, predictors 1-7
      o.jpeg_lossless_predictor = (uint8_t)(1 + r.below(7));
      o.jpeg_arithmetic = mode == 6;
      sp.exact = o.jpeg_precision == 0;
      break;
    case 7:
    case 8: // hierarchical
      o.jpeg_hierarchical_levels = (uint8_t)(1 + r.below(3));
      o.jpeg_arithmetic = mode == 8;
      break;
    case 9:
    case 10: // non-interleaved
      o.jpeg_non_interleaved = 1;
      o.jpeg_arithmetic = mode == 10;
      break;
    case 11: // abbreviated, image data only
      o.jpeg_abbreviated = 1;
      o.jpeg_arithmetic = arith;
      o.jpeg_progressive = (uint8_t)r.below(2);
      sp.needs_tables = true;
      break;
    default: // abbreviated, tables only
      o.jpeg_abbreviated = 2;
      o.jpeg_arithmetic = arith;
      sp.tables_only = true;
      break;
  }
}

static inline void fill_png(FuzzRng & r, FuzzSaveSpec & sp) {
  GIMG_Save_Options & o = sp.opts;
  exif_thumb(r, o);
  o.png_interlaced = r.below(2);
  o.png_filter = (uint8_t)r.below(6);
  o.png_palette = (uint8_t)(r.below(8) == 0 ? 2 : r.below(2));
  sp.exact = true;
}

static inline void fill_tiff(FuzzRng & r, FuzzSaveSpec & sp) {
  GIMG_Save_Options & o = sp.opts;
  o.tiff_compression = (uint8_t)r.below(4);
  o.tiff_predictor = r.below(2) ? 2u : (r.one_in(2) ? 1u : 0u);
  if (o.tiff_compression == 0u && !r.one_in(8)) {
    o.tiff_predictor = 0; // a predictor with nothing behind it is refused
  }
  o.tiff_big_endian = (uint8_t)r.below(2);
  o.tiff_bigtiff = (uint8_t)r.below(3);
  static const uint32_t rows[] = {0, 0, 1, 2, 3, 7, 64, 100000};
  o.tiff_rows_per_strip = rows[r.below(8)];
  sp.exact = true;
}

static inline void fill_gif(FuzzRng & r, FuzzSaveSpec & sp) {
  GIMG_Save_Options & o = sp.opts;
  o.gif_interlace = (uint8_t)r.below(2);
  static const uint16_t thr[] = {0, 1, 64, 128, 128, 255, 256};
  o.gif_alpha_threshold = thr[r.below(7)];
  o.gif_loop_count = r.one_in(3) ? (uint16_t)r.below(5) : 0;
  sp.exact = true;
}

static inline void fill_bmp(FuzzRng & r, FuzzSaveSpec & sp) {
  GIMG_Save_Options & o = sp.opts;
  o.bmp_palette = (uint8_t)r.below(2);
  o.bmp_rle = (uint8_t)(r.one_in(2) ? GIMG_BMP_RLE_AUTO : GIMG_BMP_RLE_NEVER);
  o.bmp_allow_2bit = (uint8_t)r.below(2);
  o.bmp_allow_rle24 = (uint8_t)r.below(2);
  o.bmp_allow_huffman = (uint8_t)r.below(2);
  uint32_t w = r.below(8);
  o.bmp_wrapper = w == 0 ? GIMG_BMP_WRAPPER_JPEG : w == 1 ? GIMG_BMP_WRAPPER_PNG
                                                          : GIMG_BMP_WRAPPER_NONE;
  o.bmp_top_down = (uint8_t)r.below(2);
  sp.exact = o.bmp_wrapper != GIMG_BMP_WRAPPER_JPEG;
}

static inline void fill_ico(FuzzRng & r, FuzzSaveSpec & sp) {
  sp.opts.ico_payload = (uint8_t)r.below(3);
}

static inline void fill_webp(FuzzRng & r, FuzzSaveSpec & sp, bool lossy) {
  GIMG_Save_Options & o = sp.opts;
  o.webp_lossless = lossy ? GIMG_WEBP_COMPRESS_LOSSY : GIMG_WEBP_COMPRESS_LOSSLESS;
  o.webp_effort = (uint8_t)r.below(10);
  o.webp_exact = (uint8_t)r.below(2);
  sp.exact = !lossy;
  sp.alpha0_free = !o.webp_exact;
}

} // namespace fuzz_detail

/**
 * The save options for one selector.
 *
 * @p variant is 0 for every format except WebP, where 0 is the lossless
 * writer and 1 the lossy one: both are written for every unit, since they
 * share a container and nothing else.
 */
static inline void fuzz_fill_spec(FuzzSaveSpec & sp, const char * fmt,
    uint32_t selector, int variant = 0) {
  FuzzRng r(selector * 31u + (uint32_t)variant);
  sp.opts.metadata_policy = fuzz_detail::kPolicies[r.below(6)];
  const std::string_view f(fmt);
  if (f == "jpeg") {
    fuzz_detail::fill_jpeg(r, sp);
  } else if (f == "png") {
    fuzz_detail::fill_png(r, sp);
  } else if (f == "tiff") {
    fuzz_detail::fill_tiff(r, sp);
  } else if (f == "gif") {
    fuzz_detail::fill_gif(r, sp);
  } else if (f == "bmp") {
    fuzz_detail::fill_bmp(r, sp);
  } else if (f == "ico") {
    fuzz_detail::fill_ico(r, sp);
  } else if (f == "webp") {
    fuzz_detail::fill_webp(r, sp, variant == 1);
  }
  // opts.jpeg_progressive_config points into sp, so a spec is filled in place
  // and never copied afterwards.
}

namespace fuzz_detail {

static inline bool same_format(
    const GIMG_Pixel_Format * a, const GIMG_Pixel_Format * b) {
  return a->channel_model == b->channel_model &&
      a->channel_type == b->channel_type && a->layout == b->layout &&
      a->channel_count == b->channel_count &&
      a->alpha_form == b->alpha_form &&
      std::memcmp(a->bits_per_channel, b->bits_per_channel, 8) == 0;
}

/** Rows of two decoded pictures, equal where they are comparable at all.
 * Returns 1 equal, 0 different, -1 not comparable (shape or format). */
static inline int compare_rasters(const GIMG_Raster * a, const GIMG_Raster * b,
    bool alpha0_free, char * where = nullptr, size_t where_size = 0) {
  if (a == nullptr || b == nullptr ||
      gimg_raster_width(a) != gimg_raster_width(b) ||
      gimg_raster_height(a) != gimg_raster_height(b) ||
      !same_format(gimg_raster_format(a), gimg_raster_format(b))) {
    return -1;
  }
  const GIMG_Pixel_Format * f = gimg_raster_format(a);
  const size_t bpp = gimg_raster_bytes_per_pixel(f);
  const size_t row = bpp * gimg_raster_width(a);
  const uint8_t * pa = (const uint8_t *)gimg_raster_pixels_const(a);
  const uint8_t * pb = (const uint8_t *)gimg_raster_pixels_const(b);
  const bool has_alpha = f->channel_model == GIMG_CHANNEL_RGBA;
  const size_t ca = has_alpha && f->channel_count != 0 ? bpp / f->channel_count : 0;
  for (uint32_t y = 0; y < gimg_raster_height(a); y++) {
    const uint8_t * ra = pa + (size_t)y * gimg_raster_stride_bytes(a);
    const uint8_t * rb = pb + (size_t)y * gimg_raster_stride_bytes(b);
    if (std::memcmp(ra, rb, row) == 0) {
      continue;
    }
    if (!alpha0_free || ca == 0) {
      if (where != nullptr) {
        size_t i = 0;
        while (ra[i] == rb[i]) {
          i++;
        }
        snprintf(where, where_size, "%ux%u bpp %zu row %u byte %zu: %u vs %u",
            gimg_raster_width(a), gimg_raster_height(a), bpp, y, i, ra[i], rb[i]);
      }
      return 0;
    }
    // Pixel by pixel: a pixel whose alpha channel is all zero in both may
    // differ in colour.  RGBA puts alpha last in every format here.
    for (uint32_t x = 0; x < gimg_raster_width(a); x++) {
      const uint8_t * qa = ra + x * bpp;
      const uint8_t * qb = rb + x * bpp;
      if (std::memcmp(qa, qb, bpp) == 0) {
        continue;
      }
      bool za = true;
      bool zb = true;
      for (size_t k = 0; k < ca; k++) {
        za = za && qa[bpp - ca + k] == 0;
        zb = zb && qb[bpp - ca + k] == 0;
      }
      if (!(za && zb)) {
        if (where != nullptr) {
          snprintf(where, where_size, "%ux%u bpp %zu at %u,%u", gimg_raster_width(a),
              gimg_raster_height(a), bpp, x, y);
        }
        return 0;
      }
    }
  }
  return 1;
}

static inline uint64_t hash_bytes(const void * p, size_t n) {
  uint64_t h = 1469598103934665603ull;
  const uint8_t * b = (const uint8_t *)p;
  for (size_t i = 0; i < n; i++) {
    h = (h ^ b[i]) * 1099511628211ull;
  }
  return h;
}

/** True when a GIF can hold @p r exactly: at most 256 distinct colors, counting
 * all fully transparent pixels as one, and no alpha but 0 and 255. */
static inline bool gif_representable(const GIMG_Raster * r) {
  if (r == nullptr || !same_format(gimg_raster_format(r), &GIMG_PIXEL_RGBA8)) {
    return false;
  }
  std::set<uint32_t> colors;
  const uint8_t * p = (const uint8_t *)gimg_raster_pixels_const(r);
  for (uint32_t y = 0; y < gimg_raster_height(r); y++) {
    const uint8_t * row = p + (size_t)y * gimg_raster_stride_bytes(r);
    for (uint32_t x = 0; x < gimg_raster_width(r); x++) {
      const uint8_t * q = row + (size_t)x * 4u;
      if (q[3] != 0 && q[3] != 255) {
        return false;
      }
      colors.insert(q[3] == 0 ? 0u : (0xFF000000u | q[0] | (q[1] << 8) | (q[2] << 16)));
      if (colors.size() > 256u) {
        return false;
      }
    }
  }
  return true;
}

/** True when @p r carries no alpha that is less than fully opaque.  A format
 * with no alpha channel (JPEG) cannot hold one, so a lossless JPEG of such a
 * raster legitimately comes back opaque. */
static inline bool alpha_all_opaque(const GIMG_Raster * r) {
  const GIMG_Pixel_Format * f = gimg_raster_format(r);
  if (f->channel_model != GIMG_CHANNEL_RGBA) {
    return true;
  }
  const size_t bpp = gimg_raster_bytes_per_pixel(f);
  const size_t ca = bpp / f->channel_count;
  const uint8_t * p = (const uint8_t *)gimg_raster_pixels_const(r);
  for (uint32_t y = 0; y < gimg_raster_height(r); y++) {
    const uint8_t * row = p + (size_t)y * gimg_raster_stride_bytes(r);
    for (uint32_t x = 0; x < gimg_raster_width(r); x++) {
      const uint8_t * q = row + (size_t)x * bpp + bpp - ca;
      for (size_t k = 0; k < ca; k++) {
        if (q[k] != 0xFF) {
          return false;
        }
      }
    }
  }
  return true;
}

/** Decode every item of @p doc (to exercise the decoder) and, for item 0,
 * hand the raster back; nullptr when it did not decode.  @p limit_hit is set
 * when item 0's refusal was a limit, which says nothing about the writer. */
static inline GIMG_Raster * decode_items(
    GIMG_Doc * doc, bool * limit_hit, bool * item0_failed) {
  GIMG_Raster * first = nullptr;
  const size_t n = gimg_doc_item_count(doc);
  for (size_t i = 0; i < n; i++) {
    GIMG_Raster * raster = nullptr;
    GIMG_Result r = gimg_item_decode(
        gimg_doc_item(doc, i), fuzz_decode_options(), &raster);
    if (i == 0u) {
      if (r == GIMG_OK && raster != nullptr) {
        first = raster;
        continue;
      }
      if (item0_failed != nullptr) {
        *item0_failed = true;
      }
      if (limit_hit != nullptr && (r == GIMG_ERR_LIMIT || r == GIMG_ERR_OOM)) {
        *limit_hit = true;
      }
    }
    if (raster != nullptr) {
      gimg_raster_destroy(raster);
    }
  }
  return first;
}

} // namespace fuzz_detail

/**
 * One save of @p doc under @p spec, then the checks.  @p source is the
 * decoded raster of item 0 (or nullptr), @p items its item count.
 */
static inline void fuzz_encode_save(GIMG_Doc * doc, const char * fmt,
    const FuzzSaveSpec & spec, uint32_t selector, int variant,
    const GIMG_Raster * source, size_t items, FuzzEncodeStats * stats) {
  GIMG_Stream * out = nullptr;
  if (gimg_stream_create_memory_output(&out) != GIMG_OK || out == nullptr) {
    return;
  }
  GIMG_Save_Report report = {0, nullptr, {0}};
  const GIMG_Result sr = gimg_doc_save(doc, out, fmt, &spec.opts, &report);
  if (stats != nullptr) {
    stats->saves++;
  }
  const void * buf = nullptr;
  size_t n = 0;
  gimg_stream_output_buffer(out, &buf, &n);
  if (sr != GIMG_OK || buf == nullptr || n == 0) {
    gimg_stream_destroy(out);
    return;
  }
  if (stats != nullptr) {
    stats->saves_ok++;
    stats->digest += fuzz_detail::hash_bytes(buf, n);
  }
  if (spec.tables_only) {
    // Tables with no frame: nothing to decode, and a plain load must refuse it
    // without a fault.
    GIMG_Stream * t = nullptr;
    if (gimg_stream_create_memory(buf, n, &t) == GIMG_OK && t != nullptr) {
      GIMG_Doc * d = nullptr;
      (void)gimg_doc_load(t, fuzz_load_options(), nullptr, &d);
      if (d != nullptr) {
        gimg_doc_destroy(d);
      }
      gimg_stream_destroy(t);
    }
    gimg_stream_destroy(out);
    return;
  }

  GIMG_Stream * back = nullptr;
  GIMG_Doc * round = nullptr;
  GIMG_Result lr = GIMG_ERR_INTERNAL;
  if (gimg_stream_create_memory(buf, n, &back) == GIMG_OK && back != nullptr) {
    lr = gimg_doc_load(back, fuzz_load_options(), nullptr, &round);
    gimg_stream_destroy(back);
  }
  char msg[320];
  if (round == nullptr) {
    // A file the writer chose to write that its own reader will not open.
    // Not an error for the abbreviated image-data form, which needs tables.
    if (!spec.needs_tables && lr != GIMG_ERR_LIMIT && lr != GIMG_ERR_OOM) {
      snprintf(msg, sizeof msg,
          "%s selector %u variant %d: the output does not load (result %d)",
          fmt, (unsigned)selector, variant, (int)lr);
      gimg_stream_destroy(out);
      fuzz_fail(msg);
      return;
    }
    gimg_stream_destroy(out);
    return;
  }
  if (stats != nullptr) {
    stats->reloaded++;
  }
  bool limit_hit = false;
  bool item0_failed = false;
  GIMG_Raster * got = fuzz_detail::decode_items(round, &limit_hit, &item0_failed);
  if (spec.exact && source != nullptr && !limit_hit) {
    if (got == nullptr) {
      snprintf(msg, sizeof msg,
          "%s selector %u variant %d: a lossless output does not decode",
          fmt, (unsigned)selector, variant);
      gimg_doc_destroy(round);
      gimg_stream_destroy(out);
      fuzz_fail(msg);
      return;
    }
    // Item 0 of a multi-frame document is compared only when the shapes agree:
    // formats differ in what frame 0 of an animation is.
    if (items == gimg_doc_item_count(round)) {
      char where[120] = "";
      const int c = fuzz_detail::compare_rasters(
          source, got, spec.alpha0_free, where, sizeof where);
      if (c >= 0 && stats != nullptr) {
        stats->compared++;
      }
      if (c == 0) {
        snprintf(msg, sizeof msg,
            "%s selector %u variant %d: the picture changed across a lossless save"
            " [%s; bits %u q %u pred %u prec %u arith %u]",
            fmt, (unsigned)selector, variant, where,
            (unsigned)gimg_raster_format(source)->bits_per_channel[0],
            spec.opts.jpeg_quality, spec.opts.jpeg_lossless_predictor,
            spec.opts.jpeg_precision, spec.opts.jpeg_arithmetic);
        gimg_raster_destroy(got);
        gimg_doc_destroy(round);
        gimg_stream_destroy(out);
        fuzz_fail(msg);
        return;
      }
    }
  }
  if (got != nullptr) {
    gimg_raster_destroy(got);
  }
  gimg_doc_destroy(round);
  gimg_stream_destroy(out);
}

/**
 * One fuzz unit for the writer of @p fmt: load @p data, decode it, save it as
 * @p fmt under the options fuzz_fill_spec derives from @p selector (both compressors for WebP), and
 * check what came out.  @p max_pixels, when non-zero, skips a picture larger
 * than that (the corpus replay's bound on run time).  The harnesses call this with
 * fuzz_option_selector(data, size); the corpus replay calls it with a list.
 */
static inline void fuzz_encode_unit(const char * fmt, const uint8_t * data,
    size_t size, uint32_t selector, FuzzEncodeStats * stats = nullptr,
    uint64_t max_pixels = 0) {
  GIMG_Stream * s = nullptr;
  if (gimg_stream_create_memory(data, size, &s) != GIMG_OK || s == nullptr) {
    return;
  }
  GIMG_Doc * doc = nullptr;
  GIMG_Result r = gimg_doc_load(s, fuzz_load_options(), nullptr, &doc);
  gimg_stream_destroy(s);
  if (r != GIMG_OK || doc == nullptr) {
    return;
  }
  // Decode every item, and attach the first raster so the save takes pixels
  // rather than re-decoding.  The raster stays owned by the item; a second
  // pointer to it is read only for the comparison.
  bool limit_hit = false;
  bool failed = false;
  GIMG_Raster * first = fuzz_detail::decode_items(doc, &limit_hit, &failed);
  const size_t items = gimg_doc_item_count(doc);
  if (max_pixels != 0 && first != nullptr &&
      (uint64_t)gimg_raster_width(first) * gimg_raster_height(first) > max_pixels) {
    gimg_raster_destroy(first); // not attached to the item yet
    gimg_doc_destroy(doc);
    return; // the replay's bound on run time; the fuzzers pass 0
  }
  const GIMG_Raster * source = nullptr;
  if (first != nullptr) {
    gimg_item_set_raster(gimg_doc_item(doc, 0), first);
    source = gimg_item_raster(gimg_doc_item(doc, 0));
  }
  const bool gif_ok = std::string_view(fmt) != "gif" ||
      fuzz_detail::gif_representable(source);
  const int variants = std::string_view(fmt) == "webp" ? 2 : 1;
  for (int v = 0; v < variants; v++) {
    FuzzSaveSpec spec;
    fuzz_fill_spec(spec, fmt, selector, v);
    // GIF holds 256 colors and one bit of alpha: a picture beyond that is
    // quantized or thresholded by design, so only one inside it is compared,
    // and then the colour under a transparent pixel is the writer's to choose.
    spec.exact = spec.exact && gif_ok;
    if (std::string_view(fmt) == "jpeg" && source != nullptr) {
      spec.exact = spec.exact && fuzz_detail::alpha_all_opaque(source);
    }
    if (std::string_view(fmt) == "gif") {
      spec.alpha0_free = true;
    }
    fuzz_encode_save(doc, fmt, spec, selector, v, source, items, stats);
  }
  gimg_doc_destroy(doc);
}

#endif // GIMG_TESTS_FUZZ_FUZZ_ENCODE_H

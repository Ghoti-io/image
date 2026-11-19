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
 * The limits the operations and the raster constructors take.
 *
 * Every operation in ops.h that allocates can be held to a GIMG_Limits: through
 * the `limits` field of its options struct, or through a `_with_limits`
 * variant. The raster constructors and the copies have the same variants. All
 * of them open the memory scope a load opens, so what is counted is the sum of
 * the live blocks the call itself takes. The tests use an allocator that knows
 * the size of every live block, measure the peak of each call, and require of
 * each variant that
 *
 *  - a max_memory equal to the measured peak succeeds, and one byte less is
 *    GIMG_ERR_LIMIT, at every cap from the smallest to the peak;
 *  - a refused call leaves nothing allocated, hands back no raster, and leaves
 *    a raster it works on in place exactly as it was;
 *  - max_decoded_pixels equal to the size of the result succeeds and one pixel
 *    less is GIMG_ERR_LIMIT, with nothing allocated at all;
 *  - the plain function, and the variant given NULL limits, are unchanged: not
 *    held to a cap of their own, and counted by a scope they run inside; and
 *  - nested in another scope, the call is held to both caps.
 */

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <ghoti.io/color/color.h>
#include <ghoti.io/image/doc.h>
#include <ghoti.io/image/meta.h>
#include <ghoti.io/image/ops.h>
#include <ghoti.io/image/raster.h>
#include <ghoti.io/image/stream.h>
#include <gtest/gtest.h>
#include <string>
#include <utility>
#include <vector>

#include "../../src/core/budget_internal.h"

namespace {

/** An allocator that records the live total and its high-water mark. */
struct Track {
  GIMG_Allocator a{};
  size_t live = 0;
  size_t peak = 0;
  static constexpr size_t kHead = 16;

  static void * m(void * ctx, size_t n) {
    Track * t = static_cast<Track *>(ctx);
    uint8_t * p = static_cast<uint8_t *>(std::malloc(n + kHead));
    if (!p) {
      return nullptr;
    }
    std::memcpy(p, &n, sizeof(n));
    t->live += n;
    if (t->live > t->peak) {
      t->peak = t->live;
    }
    return p + kHead;
  }
  static void * c(void * ctx, size_t n, size_t sz) {
    if (sz != 0 && n > SIZE_MAX / sz) {
      return nullptr;
    }
    void * p = m(ctx, n * sz);
    if (p) {
      std::memset(p, 0, n * sz);
    }
    return p;
  }
  static void f(void * ctx, void * p) {
    if (!p) {
      return;
    }
    Track * t = static_cast<Track *>(ctx);
    uint8_t * base = static_cast<uint8_t *>(p) - kHead;
    size_t n;
    std::memcpy(&n, base, sizeof(n));
    t->live -= n;
    std::free(base);
  }
  static void * r(void * ctx, void * p, size_t n) {
    if (!p) {
      return m(ctx, n);
    }
    Track * t = static_cast<Track *>(ctx);
    uint8_t * base = static_cast<uint8_t *>(p) - kHead;
    size_t old;
    std::memcpy(&old, base, sizeof(old));
    uint8_t * q = static_cast<uint8_t *>(std::realloc(base, n + kHead));
    if (!q) {
      return nullptr;
    }
    std::memcpy(q, &n, sizeof(n));
    t->live = t->live - old + n;
    if (t->live > t->peak) {
      t->peak = t->live;
    }
    return q + kHead;
  }
  Track() {
    a.ctx = this;
    a.malloc_fn = m;
    a.calloc_fn = c;
    a.realloc_fn = r;
    a.free_fn = f;
  }
};

constexpr size_t kHuge = static_cast<size_t>(1) << 40;

GIMG_Limits memory(size_t cap) {
  GIMG_Limits l = {};
  l.max_memory = cap;
  l.max_decoded_pixels = GIMG_LIMIT_NONE;
  return l;
}

GIMG_Limits pixels(size_t n) {
  GIMG_Limits l = {};
  l.max_memory = GIMG_LIMIT_NONE;
  l.max_decoded_pixels = n;
  return l;
}

/** A noisy RGBA picture, or a few flat colours when @p flat. */
void fill(uint8_t * p, size_t stride, uint32_t w, uint32_t h, bool flat) {
  uint32_t s = 20260101u;
  for (uint32_t y = 0; y < h; ++y) {
    for (uint32_t x = 0; x < w; ++x) {
      uint8_t * q = p + static_cast<size_t>(y) * stride + x * 4u;
      if (flat) {
        const uint32_t k = ((x / 5u) + (y / 4u)) % 8u;
        q[0] = static_cast<uint8_t>(k * 32u);
        q[1] = static_cast<uint8_t>(255u - k * 32u);
        q[2] = static_cast<uint8_t>(k * 7u);
        q[3] = 255;
      }
      else {
        for (int c = 0; c < 3; ++c) {
          s = s * 1664525u + 1013904223u;
          q[c] = static_cast<uint8_t>(x * 255u / w + ((s >> 24) & 15u));
        }
        q[3] = 255;
      }
    }
  }
}

GIMG_Raster * make_raster(Track & t, uint32_t w, uint32_t h, bool flat = false) {
  GIMG_Raster * r = nullptr;
  EXPECT_EQ(gimg_raster_create_with_allocator(&t.a, w, h, &GIMG_PIXEL_RGBA8,
                GIMG_RASTER_OWNED, nullptr, 0, &r),
      GIMG_OK);
  fill(static_cast<uint8_t *>(gimg_raster_pixels(r)),
      gimg_raster_stride_bytes(r), w, h, flat);
  return r;
}

/** The pixels of a raster, row by row without the padding, with its shape. */
std::vector<uint8_t> snapshot(const GIMG_Raster * r) {
  std::vector<uint8_t> v;
  const size_t row = static_cast<size_t>(gimg_raster_width(r)) *
      gimg_raster_bytes_per_pixel(gimg_raster_format(r));
  const uint8_t * p = static_cast<const uint8_t *>(gimg_raster_pixels_const(r));
  for (uint32_t y = 0; y < gimg_raster_height(r); ++y) {
    v.insert(v.end(), p + y * gimg_raster_stride_bytes(r),
        p + y * gimg_raster_stride_bytes(r) + row);
  }
  v.push_back(static_cast<uint8_t>(gimg_raster_width(r)));
  v.push_back(static_cast<uint8_t>(gimg_raster_height(r)));
  return v;
}

// ------------------------------------------------------------------ ops ----

struct Env {
  GIMG_Raster * src;
  uint32_t w;
  uint32_t h;
};

struct Case {
  std::string name;
  /** Run with these limits (NULL for none); a raster made goes in *out. */
  std::function<GIMG_Result(const Env &, const GIMG_Limits *, GIMG_Raster **)>
      run;
  /** The plain function, where the case has a `_with_limits` variant. */
  std::function<GIMG_Result(const Env &, GIMG_Raster **)> plain;
  /** Pixels in the result (or the raster rebuilt), 0 if there is no cap on it. */
  std::function<size_t(uint32_t, uint32_t)> result_pixels;
  std::function<void(GIMG_Raster *)> prepare;
  bool flat = false; ///< The source has few colours.
};

GIMG_Resize_Options resize_options(const GIMG_Limits * l, bool linear) {
  GIMG_Resize_Options o;
  gimg_resize_options_default(&o);
  if (linear) {
    o.space = GIMG_RESAMPLE_SPACE_LINEAR;
  }
  o.limits = l;
  return o;
}

/** The palette of the noisy picture, built once and outside every measurement. */
GIMG_Palette noisy_palette(uint32_t w, uint32_t h) {
  GIMG_Raster * r = nullptr;
  EXPECT_EQ(gimg_raster_create(w, h, &GIMG_PIXEL_RGBA8, GIMG_RASTER_OWNED,
                nullptr, 0, &r),
      GIMG_OK);
  fill(static_cast<uint8_t *>(gimg_raster_pixels(r)),
      gimg_raster_stride_bytes(r), w, h, false);
  GIMG_Palette p;
  std::memset(&p, 0, sizeof(p));
  const GIMG_Raster * rs[1] = {r};
  EXPECT_EQ(gimg_ops_palette_build(rs, 1, nullptr, &p), GIMG_OK);
  gimg_raster_destroy(r);
  return p;
}

constexpr uint32_t kW = 48;
constexpr uint32_t kH = 36;

std::vector<Case> cases() {
  std::vector<Case> v;
  const GIMG_Palette palette = noisy_palette(kW, kH);
  auto all = [](uint32_t w, uint32_t h) { return static_cast<size_t>(w) * h; };
  auto half = [](uint32_t w, uint32_t h) {
    return static_cast<size_t>(w / 2) * (h / 2);
  };
  auto none = [](uint32_t, uint32_t) { return size_t(0); };
  auto orient = [&](const char * name, GIMG_Orientation o) {
    Case c;
    c.name = name;
    c.run = [o](const Env & e, const GIMG_Limits * l, GIMG_Raster **) {
      return gimg_ops_apply_orientation_with_limits(e.src, o, l);
    };
    c.plain = [o](const Env & e, GIMG_Raster **) {
      return gimg_ops_apply_orientation(e.src, o);
    };
    c.result_pixels = all;
    v.push_back(c);
  };
  orient("apply_orientation transpose", GIMG_ORIENTATION_TRANSPOSE);
  orient("apply_orientation transverse", GIMG_ORIENTATION_TRANSVERSE);
  orient("apply_orientation rotate 180", GIMG_ORIENTATION_ROTATE_180);
  {
    Case c;
    c.name = "flip_horizontal";
    c.run = [](const Env & e, const GIMG_Limits * l, GIMG_Raster **) {
      return gimg_ops_flip_horizontal_with_limits(e.src, l);
    };
    c.plain = [](const Env & e, GIMG_Raster **) {
      return gimg_ops_flip_horizontal(e.src);
    };
    c.result_pixels = all;
    v.push_back(c);
  }
  {
    Case c;
    c.name = "flip_vertical";
    c.run = [](const Env & e, const GIMG_Limits * l, GIMG_Raster **) {
      return gimg_ops_flip_vertical_with_limits(e.src, l);
    };
    c.plain = [](const Env & e, GIMG_Raster **) {
      return gimg_ops_flip_vertical(e.src);
    };
    c.result_pixels = all;
    v.push_back(c);
  }
  {
    Case c;
    c.name = "rotate_90_cw";
    c.run = [](const Env & e, const GIMG_Limits * l, GIMG_Raster **) {
      return gimg_ops_rotate_90_cw_with_limits(e.src, l);
    };
    c.plain = [](const Env & e, GIMG_Raster **) {
      return gimg_ops_rotate_90_cw(e.src);
    };
    c.result_pixels = all;
    v.push_back(c);
  }
  {
    Case c;
    c.name = "rotate_90_ccw";
    c.run = [](const Env & e, const GIMG_Limits * l, GIMG_Raster **) {
      return gimg_ops_rotate_90_ccw_with_limits(e.src, l);
    };
    c.plain = [](const Env & e, GIMG_Raster **) {
      return gimg_ops_rotate_90_ccw(e.src);
    };
    c.result_pixels = all;
    v.push_back(c);
  }
  {
    Case c;
    c.name = "rotate_180";
    c.run = [](const Env & e, const GIMG_Limits * l, GIMG_Raster **) {
      return gimg_ops_rotate_180_with_limits(e.src, l);
    };
    c.plain = [](const Env & e, GIMG_Raster **) {
      return gimg_ops_rotate_180(e.src);
    };
    c.result_pixels = all;
    v.push_back(c);
  }
  {
    Case c;
    c.name = "crop half";
    c.run = [](const Env & e, const GIMG_Limits * l, GIMG_Raster ** o) {
      return gimg_ops_crop_with_limits(
          e.src, e.w / 4, e.h / 4, e.w / 2, e.h / 2, o, l);
    };
    c.plain = [](const Env & e, GIMG_Raster ** o) {
      return gimg_ops_crop(e.src, e.w / 4, e.h / 4, e.w / 2, e.h / 2, o);
    };
    c.result_pixels = half;
    v.push_back(c);
  }
  {
    Case c;
    c.name = "convert_bit_depth 8->16";
    c.run = [](const Env & e, const GIMG_Limits * l, GIMG_Raster ** o) {
      return gimg_ops_convert_bit_depth_with_limits(e.src, 16, o, l);
    };
    c.plain = [](const Env & e, GIMG_Raster ** o) {
      return gimg_ops_convert_bit_depth(e.src, 16, o);
    };
    c.result_pixels = all;
    v.push_back(c);
  }
  {
    Case c;
    c.name = "convert_pixel_format same format";
    c.run = [](const Env & e, const GIMG_Limits * l, GIMG_Raster ** o) {
      return gimg_ops_convert_pixel_format_with_limits(
          e.src, &GIMG_PIXEL_RGBA8, o, l);
    };
    c.plain = [](const Env & e, GIMG_Raster ** o) {
      return gimg_ops_convert_pixel_format(e.src, &GIMG_PIXEL_RGBA8, o);
    };
    c.result_pixels = all;
    v.push_back(c);
  }
  {
    Case c;
    c.name = "transform_color display-p3->srgb";
    c.run = [](const Env & e, const GIMG_Limits * l, GIMG_Raster ** o) {
      GIMG_Color_Transform_Options opts;
      gimg_color_transform_options_srgb(&opts);
      opts.limits = l;
      return gimg_ops_transform_color(e.src, &opts, o);
    };
    c.result_pixels = all;
    c.prepare = [](GIMG_Raster * src) {
      GCOL_Color_Info ci;
      gcol_color_info_default(&ci);
      EXPECT_TRUE(gcol_color_info_set_gamut(&ci, GCOL_PRIMARIES_DISPLAY_P3));
      ci.transfer = GCOL_TRANSFER_SRGB;
      EXPECT_EQ(gimg_raster_set_color_info(src, &ci), GIMG_OK);
    };
    v.push_back(c);
  }
  {
    Case c;
    c.name = "resize down 1/2";
    c.run = [](const Env & e, const GIMG_Limits * l, GIMG_Raster ** o) {
      const GIMG_Resize_Options opt = resize_options(l, false);
      return gimg_ops_resize(e.src, e.w / 2, e.h / 2, &opt, o);
    };
    c.result_pixels = half;
    v.push_back(c);
  }
  {
    Case c;
    c.name = "resize up 2x";
    c.run = [](const Env & e, const GIMG_Limits * l, GIMG_Raster ** o) {
      const GIMG_Resize_Options opt = resize_options(l, false);
      return gimg_ops_resize(e.src, e.w * 2, e.h * 2, &opt, o);
    };
    c.result_pixels = [](uint32_t w, uint32_t h) {
      return static_cast<size_t>(w) * 2 * h * 2;
    };
    v.push_back(c);
  }
  {
    Case c;
    c.name = "resize linear 1/3";
    c.run = [](const Env & e, const GIMG_Limits * l, GIMG_Raster ** o) {
      const GIMG_Resize_Options opt = resize_options(l, true);
      return gimg_ops_resize(e.src, e.w / 3, e.h / 3, &opt, o);
    };
    c.result_pixels = [](uint32_t w, uint32_t h) {
      return static_cast<size_t>(w / 3) * (h / 3);
    };
    v.push_back(c);
  }
  {
    Case c;
    c.name = "quantize 256";
    c.run = [](const Env & e, const GIMG_Limits * l, GIMG_Raster ** o) {
      GIMG_Quantize_Options q = {};
      q.limits = l;
      GIMG_Palette p;
      return gimg_ops_quantize(e.src, &q, o, &p);
    };
    c.result_pixels = all;
    v.push_back(c);
  }
  {
    Case c;
    c.name = "quantize 64 floyd-steinberg";
    c.run = [](const Env & e, const GIMG_Limits * l, GIMG_Raster ** o) {
      GIMG_Quantize_Options q = {};
      q.max_colors = 64;
      q.dither = GIMG_DITHER_FLOYD_STEINBERG;
      q.limits = l;
      GIMG_Palette p;
      return gimg_ops_quantize(e.src, &q, o, &p);
    };
    c.result_pixels = all;
    v.push_back(c);
  }
  {
    Case c;
    c.name = "palette_build";
    c.run = [](const Env & e, const GIMG_Limits * l, GIMG_Raster **) {
      GIMG_Quantize_Options q = {};
      q.limits = l;
      const GIMG_Raster * rs[1] = {e.src};
      GIMG_Palette p;
      return gimg_ops_palette_build(rs, 1, &q, &p);
    };
    c.plain = [](const Env & e, GIMG_Raster **) {
      GIMG_Quantize_Options q = {};
      const GIMG_Raster * rs[1] = {e.src};
      GIMG_Palette p;
      return gimg_ops_palette_build(rs, 1, &q, &p);
    };
    c.result_pixels = none;
    v.push_back(c);
  }
  {
    Case c;
    c.name = "palette_apply floyd-steinberg";
    c.run = [palette](const Env & e, const GIMG_Limits * l, GIMG_Raster ** o) {
      return gimg_ops_palette_apply_with_limits(
          e.src, &palette, GIMG_DITHER_FLOYD_STEINBERG, o, l);
    };
    c.plain = [palette](const Env & e, GIMG_Raster ** o) {
      return gimg_ops_palette_apply(
          e.src, &palette, GIMG_DITHER_FLOYD_STEINBERG, o);
    };
    c.result_pixels = all;
    v.push_back(c);
  }
  {
    Case c;
    c.name = "palette_from_raster";
    c.run = [](const Env & e, const GIMG_Limits * l, GIMG_Raster **) {
      GIMG_Palette p;
      return gimg_ops_palette_from_raster_with_limits(e.src, 256, &p, l);
    };
    c.plain = [](const Env & e, GIMG_Raster **) {
      GIMG_Palette p;
      return gimg_ops_palette_from_raster(e.src, 256, &p);
    };
    c.result_pixels = none;
    c.flat = true;
    v.push_back(c);
  }
  {
    Case c;
    c.name = "count_colors";
    c.run = [](const Env & e, const GIMG_Limits * l, GIMG_Raster **) {
      size_t n = 0;
      bool exact = false;
      return gimg_ops_count_colors_with_limits(
          e.src, 1u << 20, &n, &exact, l);
    };
    c.plain = [](const Env & e, GIMG_Raster **) {
      size_t n = 0;
      bool exact = false;
      return gimg_ops_count_colors(e.src, 1u << 20, &n, &exact);
    };
    c.result_pixels = none;
    v.push_back(c);
  }
  return v;
}

struct OpRun {
  GIMG_Result result = GIMG_OK;
  size_t peak = 0;     ///< Most live bytes the call held, above its start.
  size_t live_end = 0; ///< Bytes still live after everything is destroyed.
  bool refused_scope = false;
  bool made_raster = false;
  std::vector<uint8_t> source_after;  ///< The raster worked on, afterwards.
  std::vector<uint8_t> source_before; ///< ...and before.
  std::vector<uint8_t> result_pixels; ///< The raster made, if any.
};

/**
 * Run one case. @p limits is given to the variant; @p outer_cap, when not 0,
 * opens a scope round the call first. @p plain runs the plain function.
 */
OpRun run_case(const Case & c, uint32_t w, uint32_t h, const GIMG_Limits * limits,
    size_t outer_cap = 0, bool plain = false) {
  OpRun run;
  Track t;
  Env e{make_raster(t, w, h, c.flat), w, h};
  if (c.prepare) {
    c.prepare(e.src);
  }
  run.source_before = snapshot(e.src);
  GIMG_Raster * out = nullptr;
  const size_t base = t.live;
  t.peak = t.live;
  gimg_budget_scope_t scope;
  if (outer_cap) {
    gimg_budget_enter(&scope, outer_cap);
  }
  run.result = plain ? c.plain(e, &out) : c.run(e, limits, &out);
  if (outer_cap) {
    run.refused_scope = gimg_budget_leave(&scope);
  }
  run.peak = t.peak - base;
  run.made_raster = out != nullptr;
  if (out) {
    run.result_pixels = snapshot(out);
  }
  run.source_after = snapshot(e.src);
  gimg_raster_destroy(out);
  gimg_raster_destroy(e.src);
  run.live_end = t.live;
  return run;
}

} // namespace

TEST(OpsLimits, TheOptionsStructsDefaultToNoLimits) {
  GIMG_Resize_Options r;
  gimg_resize_options_default(&r);
  EXPECT_EQ(r.limits, nullptr);
  GIMG_Color_Transform_Options x;
  gimg_color_transform_options_default(&x);
  EXPECT_EQ(x.limits, nullptr);
  gimg_color_transform_options_srgb(&x);
  EXPECT_EQ(x.limits, nullptr);
  GIMG_Quantize_Options q = {};
  EXPECT_EQ(q.limits, nullptr);
}

TEST(OpsLimits, ACapOfThePeakSucceedsAndEveryCapBelowItIsRefusedCleanly) {
  for (const Case & c : cases()) {
    SCOPED_TRACE(c.name);
    const GIMG_Limits open = memory(kHuge);
    const OpRun measured = run_case(c, kW, kH, &open);
    ASSERT_EQ(measured.result, GIMG_OK);
    ASSERT_GT(measured.peak, 0u);
    EXPECT_EQ(measured.live_end, 0u);

    const GIMG_Limits exact = memory(measured.peak);
    const OpRun fits = run_case(c, kW, kH, &exact);
    EXPECT_EQ(fits.result, GIMG_OK) << "cap " << measured.peak;
    EXPECT_LE(fits.peak, measured.peak);
    EXPECT_EQ(fits.live_end, 0u);
    EXPECT_EQ(fits.result_pixels, measured.result_pixels);
    EXPECT_EQ(fits.source_after, measured.source_after);

    // Every cap from one byte to one under the peak: refused, with nothing
    // left over. The small ones are tried one by one, the rest in a spread.
    std::vector<size_t> caps;
    for (size_t cap = 1; cap < measured.peak && cap <= 96; ++cap) {
      caps.push_back(cap);
    }
    const size_t step = measured.peak / 200 + 1;
    for (size_t cap = 97; cap < measured.peak; cap += step) {
      caps.push_back(cap);
    }
    caps.push_back(measured.peak - 1);
    for (size_t cap : caps) {
      const GIMG_Limits tight = memory(cap);
      const OpRun refused = run_case(c, kW, kH, &tight);
      ASSERT_EQ(refused.result, GIMG_ERR_LIMIT) << "cap " << cap;
      EXPECT_LE(refused.peak, cap);
      EXPECT_EQ(refused.live_end, 0u) << "cap " << cap << ": a block leaked";
      EXPECT_FALSE(refused.made_raster) << "cap " << cap;
      EXPECT_EQ(refused.source_after, refused.source_before)
          << "cap " << cap << ": a refused call changed the raster";
    }
  }
}

TEST(OpsLimits, TheDefaultsApplyToAZeroedStructAndNoneLiftsThem) {
  for (const Case & c : cases()) {
    SCOPED_TRACE(c.name);
    const GIMG_Limits zeroed = {};
    EXPECT_EQ(run_case(c, kW, kH, &zeroed).result, GIMG_OK);
    GIMG_Limits none = {};
    none.max_memory = GIMG_LIMIT_NONE;
    none.max_decoded_pixels = GIMG_LIMIT_NONE;
    EXPECT_EQ(run_case(c, kW, kH, &none).result, GIMG_OK);
  }
}

TEST(OpsLimits, ThePixelCapIsExactAndRefusesBeforeAllocating) {
  for (const Case & c : cases()) {
    SCOPED_TRACE(c.name);
    const size_t n = c.result_pixels(kW, kH);
    if (n == 0) {
      continue; // no raster comes out of it, so no pixel count to hold
    }
    const GIMG_Limits fits = pixels(n);
    const OpRun ok = run_case(c, kW, kH, &fits);
    EXPECT_EQ(ok.result, GIMG_OK) << "cap " << n;
    const GIMG_Limits under = pixels(n - 1);
    const OpRun refused = run_case(c, kW, kH, &under);
    EXPECT_EQ(refused.result, GIMG_ERR_LIMIT) << "cap " << n - 1;
    EXPECT_EQ(refused.peak, 0u) << "something was allocated before the refusal";
    EXPECT_EQ(refused.live_end, 0u);
    EXPECT_FALSE(refused.made_raster);
    EXPECT_EQ(refused.source_after, refused.source_before);
  }
}

TEST(OpsLimits, ThePixelCapDoesNotTouchWorkThatMakesNoRaster) {
  // Counting colours and choosing a palette make no raster, so a pixel cap of
  // one has nothing to refuse; their memory is the other cap's.
  for (const Case & c : cases()) {
    if (c.result_pixels(kW, kH) != 0) {
      continue;
    }
    SCOPED_TRACE(c.name);
    const GIMG_Limits one_pixel = pixels(1);
    EXPECT_EQ(run_case(c, kW, kH, &one_pixel).result, GIMG_OK);
  }
}

TEST(OpsLimits, ThePlainFunctionAndNullLimitsAreUnchanged) {
  for (const Case & c : cases()) {
    SCOPED_TRACE(c.name);
    const OpRun with_null = run_case(c, kW, kH, nullptr);
    ASSERT_EQ(with_null.result, GIMG_OK);
    if (c.plain) {
      const OpRun plain = run_case(c, kW, kH, nullptr, 0, true);
      EXPECT_EQ(plain.result, GIMG_OK);
      EXPECT_EQ(plain.peak, with_null.peak);
      EXPECT_EQ(plain.result_pixels, with_null.result_pixels);
      EXPECT_EQ(plain.source_after, with_null.source_after);
      EXPECT_EQ(plain.live_end, 0u);
    }
    // The same call given a cap of one byte is refused, which is what shows
    // the call above was held to nothing.
    const GIMG_Limits one = memory(1);
    EXPECT_EQ(run_case(c, kW, kH, &one).result, GIMG_ERR_LIMIT);

    // Inside a scope of a load, a decode or a save, a call with no limits is
    // counted by that scope, exactly as before: the peak is allowed, a byte
    // less is refused.
    const OpRun in_ok = run_case(c, kW, kH, nullptr, with_null.peak);
    EXPECT_EQ(in_ok.result, GIMG_OK);
    EXPECT_FALSE(in_ok.refused_scope);
    const OpRun in_refused = run_case(c, kW, kH, nullptr, with_null.peak - 1);
    EXPECT_NE(in_refused.result, GIMG_OK);
    EXPECT_TRUE(in_refused.refused_scope);
    EXPECT_EQ(in_refused.live_end, 0u);
    EXPECT_FALSE(in_refused.made_raster);
    if (c.plain) {
      const OpRun plain_in = run_case(c, kW, kH, nullptr, with_null.peak - 1, true);
      EXPECT_NE(plain_in.result, GIMG_OK);
      EXPECT_TRUE(plain_in.refused_scope);
    }
  }
}

TEST(OpsLimits, ACallNestedInAScopeIsHeldToBothCaps) {
  for (const Case & c : cases()) {
    SCOPED_TRACE(c.name);
    const GIMG_Limits open = memory(kHuge);
    const OpRun measured = run_case(c, kW, kH, &open);
    ASSERT_EQ(measured.result, GIMG_OK);
    const size_t peak = measured.peak;

    // Both caps allow it.
    const GIMG_Limits exact = memory(peak);
    EXPECT_EQ(run_case(c, kW, kH, &exact, peak).result, GIMG_OK);
    // The outer is the tighter.
    const OpRun outer = run_case(c, kW, kH, &exact, peak - 1);
    EXPECT_EQ(outer.result, GIMG_ERR_LIMIT);
    EXPECT_TRUE(outer.refused_scope);
    EXPECT_EQ(outer.live_end, 0u);
    // The inner is the tighter: the outer scope never loosens it.
    const GIMG_Limits under = memory(peak - 1);
    const OpRun inner = run_case(c, kW, kH, &under, kHuge);
    EXPECT_EQ(inner.result, GIMG_ERR_LIMIT);
    EXPECT_EQ(inner.live_end, 0u);
    EXPECT_FALSE(inner.made_raster);
  }
}

TEST(OpsLimits, AnInvalidCallKeepsItsOwnError) {
  // Arguments are reported ahead of the limits, and an unsupported format is
  // not relabelled by a cap that it would have been within.
  Track t;
  GIMG_Raster * src = make_raster(t, 8, 8);
  GIMG_Raster * out = nullptr;
  const GIMG_Limits generous = memory(kHuge);
  EXPECT_EQ(gimg_ops_crop_with_limits(nullptr, 0, 0, 1, 1, &out, &generous),
      GIMG_ERR_INTERNAL);
  EXPECT_EQ(gimg_ops_crop_with_limits(src, 0, 0, 1, 1, nullptr, &generous),
      GIMG_ERR_INTERNAL);
  EXPECT_EQ(gimg_ops_crop_with_limits(src, 0, 0, 0, 1, &out, &generous),
      GIMG_ERR_INTERNAL);
  EXPECT_EQ(out, nullptr);
  EXPECT_EQ(gimg_ops_convert_bit_depth_with_limits(src, 7, &out, &generous),
      GIMG_ERR_UNSUPPORTED);
  EXPECT_EQ(gimg_ops_convert_bit_depth_with_limits(src, 16, nullptr, &generous),
      GIMG_ERR_INTERNAL);
  // The identity orientation does nothing, and is never refused.
  const GIMG_Limits tiny = memory(1);
  const GIMG_Limits no_pixels = pixels(1);
  EXPECT_EQ(gimg_ops_apply_orientation_with_limits(
                src, GIMG_ORIENTATION_NORMAL, &tiny),
      GIMG_OK);
  EXPECT_EQ(gimg_ops_apply_orientation_with_limits(
                src, GIMG_ORIENTATION_NORMAL, &no_pixels),
      GIMG_OK);
  gimg_raster_destroy(src);
  EXPECT_EQ(t.live, 0u);
}

// ------------------------------------------------------- raster and docs ----

namespace {

/** What making a raster of this size costs, record and pixels, measured. */
size_t raster_cost(uint32_t w, uint32_t h) {
  Track t;
  gimg_raster_destroy(make_raster(t, w, h));
  return t.peak;
}

/** What a constructor call held, above its start. */
struct Made {
  GIMG_Result result = GIMG_OK;
  size_t peak = 0;
  size_t live_end = 0;
  bool made = false;
};

using Make = std::function<GIMG_Result(Track &, const GIMG_Limits *, void **)>;
using Free = std::function<void(void *)>;

Made make_with(const Make & make, const Free & release, const GIMG_Limits * l,
    Track & t) {
  Made m;
  void * out = nullptr;
  const size_t base = t.live;
  t.peak = t.live;
  m.result = make(t, l, &out);
  m.peak = t.peak - base;
  m.made = out != nullptr;
  release(out);
  m.live_end = t.live - base;
  return m;
}

/**
 * The sweep every constructor goes through: the measured peak is allowed, any
 * cap under it is GIMG_ERR_LIMIT with nothing made and nothing left over.
 * @p largest_raster_pixels is the pixel count a cap must admit and, one
 * under, refuse. A raster constructor refuses it before it allocates; a
 * document call has made the document by then, and unwinds it.
 */
void sweep(const char * name, const Make & make, const Free & release,
    size_t largest_raster_pixels, Track & t, bool refuses_before_allocating) {
  SCOPED_TRACE(name);
  const GIMG_Limits open = memory(kHuge);
  const Made measured = make_with(make, release, &open, t);
  ASSERT_EQ(measured.result, GIMG_OK);
  ASSERT_TRUE(measured.made);
  ASSERT_GT(measured.peak, 0u);
  EXPECT_EQ(measured.live_end, 0u);

  const GIMG_Limits exact = memory(measured.peak);
  const Made fits = make_with(make, release, &exact, t);
  EXPECT_EQ(fits.result, GIMG_OK) << "cap " << measured.peak;
  EXPECT_LE(fits.peak, measured.peak);
  EXPECT_EQ(fits.live_end, 0u);

  // The unlimited call, and the call with no limits, take the same.
  const Made plain = make_with(make, release, nullptr, t);
  EXPECT_EQ(plain.result, GIMG_OK);
  EXPECT_EQ(plain.peak, measured.peak);

  for (size_t cap = 0; cap < measured.peak; ++cap) {
    GIMG_Limits tight = memory(cap);
    if (cap == 0) {
      // Zero is the default, not a cap of nothing: the call succeeds.
      EXPECT_EQ(make_with(make, release, &tight, t).result, GIMG_OK);
      continue;
    }
    const Made refused = make_with(make, release, &tight, t);
    ASSERT_EQ(refused.result, GIMG_ERR_LIMIT) << "cap " << cap;
    EXPECT_LE(refused.peak, cap);
    EXPECT_FALSE(refused.made) << "cap " << cap;
    ASSERT_EQ(refused.live_end, 0u) << "cap " << cap << ": a block leaked";
  }

  const GIMG_Limits px_fits = pixels(largest_raster_pixels);
  EXPECT_EQ(make_with(make, release, &px_fits, t).result, GIMG_OK);
  const GIMG_Limits px_under = pixels(largest_raster_pixels - 1);
  const Made px = make_with(make, release, &px_under, t);
  EXPECT_EQ(px.result, GIMG_ERR_LIMIT);
  if (refuses_before_allocating) {
    EXPECT_EQ(px.peak, 0u) << "something was allocated before the refusal";
  }
  EXPECT_FALSE(px.made);
  EXPECT_EQ(px.live_end, 0u);
}

void free_raster(void * p) {
  gimg_raster_destroy(static_cast<GIMG_Raster *>(p));
}
void free_doc(void * p) {
  gimg_doc_destroy(static_cast<GIMG_Doc *>(p));
}

} // namespace

TEST(RasterLimits, CreateIsHeldToMemoryAndPixels) {
  Track t;
  for (const auto & dims : std::vector<std::pair<uint32_t, uint32_t>>{
           {2, 1}, {7, 5}, {40, 30}}) {
    const uint32_t w = dims.first;
    const uint32_t h = dims.second;
    sweep("create_with_allocator_and_limits",
        [&](Track & tr, const GIMG_Limits * l, void ** out) {
          return gimg_raster_create_with_allocator_and_limits(&tr.a, l, w, h,
              &GIMG_PIXEL_RGBA8, GIMG_RASTER_OWNED, nullptr, 0,
              reinterpret_cast<GIMG_Raster **>(out));
        },
        free_raster, static_cast<size_t>(w) * h, t, true);
    EXPECT_EQ(t.live, 0u);
  }
}

TEST(RasterLimits, CreateWithLimitsUsesTheDefaultAllocatorAndNullIsPlain) {
  GIMG_Raster * r = nullptr;
  const GIMG_Limits l = memory(4096);
  ASSERT_EQ(gimg_raster_create_with_limits(&l, 16, 16, &GIMG_PIXEL_GRAY8,
                GIMG_RASTER_OWNED, nullptr, 0, &r),
      GIMG_OK);
  ASSERT_NE(r, nullptr);
  EXPECT_EQ(gimg_raster_width(r), 16u);
  gimg_raster_destroy(r);
  // The pixels alone are 256 bytes, so a cap of 200 refuses them.
  const GIMG_Limits small = memory(200);
  r = reinterpret_cast<GIMG_Raster *>(1);
  EXPECT_EQ(gimg_raster_create_with_limits(&small, 16, 16, &GIMG_PIXEL_GRAY8,
                GIMG_RASTER_OWNED, nullptr, 0, &r),
      GIMG_ERR_LIMIT);
  EXPECT_EQ(r, nullptr);
  ASSERT_EQ(gimg_raster_create_with_limits(nullptr, 16, 16, &GIMG_PIXEL_GRAY8,
                GIMG_RASTER_OWNED, nullptr, 0, &r),
      GIMG_OK);
  gimg_raster_destroy(r);
}

TEST(RasterLimits, ABorrowedBufferTakesOnlyTheRecordButIsHeldToThePixelCap) {
  Track t;
  std::vector<uint8_t> buffer(40 * 30 * 4);
  GIMG_Raster * r = nullptr;
  const GIMG_Limits open = memory(kHuge);
  ASSERT_EQ(gimg_raster_create_with_allocator_and_limits(&t.a, &open, 40, 30,
                &GIMG_PIXEL_RGBA8, GIMG_RASTER_BORROWED, buffer.data(), 0, &r),
      GIMG_OK);
  EXPECT_LT(t.peak, buffer.size()) << "the borrowed pixels were counted";
  gimg_raster_destroy(r);
  const GIMG_Limits under = pixels(40 * 30 - 1);
  r = reinterpret_cast<GIMG_Raster *>(1);
  EXPECT_EQ(gimg_raster_create_with_allocator_and_limits(&t.a, &under, 40, 30,
                &GIMG_PIXEL_RGBA8, GIMG_RASTER_BORROWED, buffer.data(), 0, &r),
      GIMG_ERR_LIMIT);
  EXPECT_EQ(r, nullptr);
  EXPECT_EQ(t.live, 0u);
}

TEST(RasterLimits, ADefaultLimitsRefusesAHugeRasterWithoutAllocatingIt) {
  // 2^29 pixels is over the default pixel cap, and over the default memory
  // cap as bytes. The plain constructor would try to take two gibibytes; the
  // limited one never asks the allocator for anything.
  Track t;
  GIMG_Raster * r = nullptr;
  const GIMG_Limits defaults = {};
  EXPECT_EQ(gimg_raster_create_with_allocator_and_limits(&t.a, &defaults,
                1u << 15, 1u << 14, &GIMG_PIXEL_RGBA8, GIMG_RASTER_OWNED,
                nullptr, 0, &r),
      GIMG_ERR_LIMIT);
  EXPECT_EQ(r, nullptr);
  EXPECT_EQ(t.peak, 0u);
  // Lifting the pixel cap leaves the memory cap, which refuses it as bytes (the
  // raster record is taken first and given back).
  GIMG_Limits lifted = {};
  lifted.max_decoded_pixels = GIMG_LIMIT_NONE;
  EXPECT_EQ(gimg_raster_create_with_allocator_and_limits(&t.a, &lifted,
                1u << 15, 1u << 14, &GIMG_PIXEL_RGBA8, GIMG_RASTER_OWNED,
                nullptr, 0, &r),
      GIMG_ERR_LIMIT);
  EXPECT_EQ(r, nullptr);
  EXPECT_EQ(t.live, 0u);
  // Argument errors are reported as themselves.
  EXPECT_EQ(gimg_raster_create_with_allocator_and_limits(&t.a, &defaults, 0, 4,
                &GIMG_PIXEL_RGBA8, GIMG_RASTER_OWNED, nullptr, 0, &r),
      GIMG_ERR_INTERNAL);
}

TEST(RasterLimits, CopyIsHeldToMemoryAndPixelsAndCarriesItsProfile) {
  Track t;
  GIMG_Raster * src = make_raster(t, 40, 30);
  const unsigned char profile[300] = {1, 2, 3};
  GCOL_Color_Info ci;
  gcol_color_info_default(&ci);
  ci.transfer = GCOL_TRANSFER_SRGB;
  ci.icc_bytes = profile;
  ci.icc_size = sizeof(profile);
  ASSERT_EQ(gimg_raster_set_color_info(src, &ci), GIMG_OK);
  sweep("copy_with_allocator_and_limits",
      [&](Track & tr, const GIMG_Limits * l, void ** out) {
        return gimg_raster_copy_with_allocator_and_limits(
            &tr.a, l, src, reinterpret_cast<GIMG_Raster **>(out));
      },
      free_raster, 40 * 30, t, true);
  // The copy's profile is in the peak: a cap that holds the record and the
  // pixels but not the profile is refused.
  const GIMG_Limits no_room = memory(raster_cost(40, 30) + 100);
  GIMG_Raster * out = nullptr;
  EXPECT_EQ(gimg_raster_copy_with_allocator_and_limits(
                &t.a, &no_room, src, &out),
      GIMG_ERR_LIMIT);
  EXPECT_EQ(out, nullptr);
  // The default-allocator form and the plain function.
  const GIMG_Limits open = memory(kHuge);
  ASSERT_EQ(gimg_raster_copy_with_limits(&open, src, &out), GIMG_OK);
  gimg_raster_destroy(out);
  ASSERT_EQ(gimg_raster_copy_with_limits(nullptr, src, &out), GIMG_OK);
  gimg_raster_destroy(out);
  ASSERT_EQ(gimg_raster_copy(src, &out), GIMG_OK);
  gimg_raster_destroy(out);
  gimg_raster_destroy(src);
  EXPECT_EQ(t.live, 0u);
}

TEST(DocLimits, FromRasterIsHeldToMemoryAndPixels) {
  Track t;
  GIMG_Raster * src = make_raster(t, 40, 30);
  sweep("doc_from_raster_with_allocator_and_limits",
      [&](Track & tr, const GIMG_Limits * l, void ** out) {
        return gimg_doc_from_raster_with_allocator_and_limits(
            &tr.a, l, src, reinterpret_cast<GIMG_Doc **>(out));
      },
      free_doc, 40 * 30, t, false);
  GIMG_Doc * doc = nullptr;
  const GIMG_Limits open = memory(kHuge);
  ASSERT_EQ(gimg_doc_from_raster_with_limits(&open, src, &doc), GIMG_OK);
  gimg_doc_destroy(doc);
  ASSERT_EQ(gimg_doc_from_raster_with_limits(nullptr, src, &doc), GIMG_OK);
  gimg_doc_destroy(doc);
  gimg_raster_destroy(src);
  EXPECT_EQ(t.live, 0u);
}

namespace {

/** A document of three frames of different sizes, with a description. */
GIMG_Doc * make_doc(Track & t) {
  GIMG_Doc * doc = nullptr;
  EXPECT_EQ(gimg_doc_create_with_allocator(&t.a, &doc), GIMG_OK);
  EXPECT_EQ(gimg_doc_set_item_count(doc, 3), GIMG_OK);
  const uint32_t sizes[3][2] = {{20, 10}, {40, 30}, {16, 8}};
  for (size_t i = 0; i < 3; ++i) {
    gimg_item_set_raster(gimg_doc_item(doc, i),
        make_raster(t, sizes[i][0], sizes[i][1]));
    gimg_item_set_frame_delay(gimg_doc_item(doc, i), 1, 10);
  }
  GIMG_Meta_Common * meta = nullptr;
  EXPECT_EQ(gimg_doc_ensure_meta_common(doc, &meta), GIMG_OK);
  EXPECT_EQ(gimg_meta_common_set_description(meta, "a description"), GIMG_OK);
  return doc;
}

} // namespace

TEST(DocLimits, CopyIsHeldToTheSumOfTheWholeDocumentAndToEachRastersPixels) {
  Track t;
  GIMG_Doc * src = make_doc(t);
  sweep("doc_copy_with_allocator_and_limits",
      [&](Track & tr, const GIMG_Limits * l, void ** out) {
        return gimg_doc_copy_with_allocator_and_limits(
            &tr.a, l, src, reinterpret_cast<GIMG_Doc **>(out));
      },
      free_doc, 40 * 30, t, false);
  // The sum is the point: the largest raster alone is far under the peak.
  const GIMG_Limits one_raster = memory(raster_cost(40, 30) + 64);
  GIMG_Doc * out = nullptr;
  EXPECT_EQ(gimg_doc_copy_with_allocator_and_limits(
                &t.a, &one_raster, src, &out),
      GIMG_ERR_LIMIT);
  EXPECT_EQ(out, nullptr);

  const GIMG_Limits open = memory(kHuge);
  ASSERT_EQ(gimg_doc_copy_with_limits(&open, src, &out), GIMG_OK);
  EXPECT_EQ(gimg_doc_item_count(out), 3u);
  EXPECT_STREQ(gimg_meta_common_description(gimg_doc_meta_common(out)),
      "a description");
  gimg_doc_destroy(out);
  ASSERT_EQ(gimg_doc_copy_with_limits(nullptr, src, &out), GIMG_OK);
  gimg_doc_destroy(out);
  ASSERT_EQ(gimg_doc_copy(src, &out), GIMG_OK);
  gimg_doc_destroy(out);
  gimg_doc_destroy(src);
  EXPECT_EQ(t.live, 0u);
}

TEST(DocLimits, ItemCopyIsHeldAndALimitedRefusalLeavesTheTargetsRasterAlone) {
  Track t;
  GIMG_Doc * src = make_doc(t);
  GIMG_Doc * dst = make_doc(t);
  GIMG_Item * si = gimg_doc_item(src, 1);
  GIMG_Item * di = gimg_doc_item(dst, 0);
  GIMG_Raster * before = gimg_item_raster(di);

  const GIMG_Limits open = memory(kHuge);
  // Measure on a copy into a fresh document's item, destroying what it made.
  GIMG_Doc * probe = make_doc(t);
  const size_t probe_base = t.live;
  t.peak = t.live;
  ASSERT_EQ(gimg_item_copy_with_limits(si, gimg_doc_item(probe, 0), &open),
      GIMG_OK);
  const size_t peak = t.peak - probe_base;
  EXPECT_GE(peak, 40u * 30u * 4u);
  gimg_doc_destroy(probe);

  const GIMG_Limits exact = memory(peak);
  ASSERT_EQ(gimg_item_copy_with_limits(si, di, &exact), GIMG_OK);
  EXPECT_NE(gimg_item_raster(di), before);
  EXPECT_EQ(gimg_raster_width(gimg_item_raster(di)), 40u);

  GIMG_Item * di2 = gimg_doc_item(dst, 2);
  GIMG_Raster * before2 = gimg_item_raster(di2);
  const GIMG_Limits tight = memory(peak - 1);
  EXPECT_EQ(gimg_item_copy_with_limits(si, di2, &tight), GIMG_ERR_LIMIT);
  EXPECT_EQ(gimg_item_raster(di2), before2);
  const GIMG_Limits px = pixels(40 * 30 - 1);
  EXPECT_EQ(gimg_item_copy_with_limits(si, di2, &px), GIMG_ERR_LIMIT);
  EXPECT_EQ(gimg_item_raster(di2), before2);
  EXPECT_EQ(gimg_item_copy_with_limits(si, di2, nullptr), GIMG_OK);

  gimg_doc_destroy(src);
  gimg_doc_destroy(dst);
  EXPECT_EQ(t.live, 0u);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}

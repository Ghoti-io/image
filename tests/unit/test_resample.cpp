/**
 * @file
 *
 * Unit tests for gimg_ops_resize().
 *
 * The comparison against an outside resampler lives in
 * tests/data/verify_resample.py, because it needs Pillow.  What is here is
 * the set of properties that can be checked without any oracle at all - and
 * those matter more, because they catch the failures that still look like a
 * picture.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <ghoti.io/image/color.h>
#include <ghoti.io/image/ops.h>
#include <ghoti.io/image/raster.h>
#include <cmath>
#include <cstring>
#include <gtest/gtest.h>
#include <vector>

namespace {

const GIMG_Resample_Filter kAveragingFilters[] = {GIMG_FILTER_BOX,
    GIMG_FILTER_TRIANGLE, GIMG_FILTER_CATMULL_ROM, GIMG_FILTER_LANCZOS3};

const GIMG_Resample_Filter kAllFilters[] = {GIMG_FILTER_AUTO,
    GIMG_FILTER_NEAREST, GIMG_FILTER_BOX, GIMG_FILTER_TRIANGLE,
    GIMG_FILTER_CATMULL_ROM, GIMG_FILTER_LANCZOS3};

const char * filter_name(GIMG_Resample_Filter f) {
  switch (f) {
    case GIMG_FILTER_AUTO: return "AUTO";
    case GIMG_FILTER_NEAREST: return "NEAREST";
    case GIMG_FILTER_BOX: return "BOX";
    case GIMG_FILTER_TRIANGLE: return "TRIANGLE";
    case GIMG_FILTER_CATMULL_ROM: return "CATMULL_ROM";
    case GIMG_FILTER_LANCZOS3: return "LANCZOS3";
    default: return "?";
  }
}

GIMG_Raster * make_raster(
    uint32_t w, uint32_t h, const GIMG_Pixel_Format * fmt) {
  GIMG_Raster * r = nullptr;
  if (gimg_raster_create(w, h, fmt, GIMG_RASTER_OWNED, nullptr, 0, &r) !=
      GIMG_OK) {
    return nullptr;
  }
  return r;
}

unsigned char * px8(GIMG_Raster * r, uint32_t x, uint32_t y, size_t bpp) {
  return (unsigned char *)gimg_raster_pixels(r) +
      (size_t)y * gimg_raster_stride_bytes(r) + (size_t)x * bpp;
}

const unsigned char * cpx8(
    const GIMG_Raster * r, uint32_t x, uint32_t y, size_t bpp) {
  return (const unsigned char *)gimg_raster_pixels_const(r) +
      (size_t)y * gimg_raster_stride_bytes(r) + (size_t)x * bpp;
}

/** A deterministic noise source; incompressible, so nothing averages it away
 *  by accident and a filter that fails to low-pass is visible. */
struct Noise {
  uint32_t state;
  explicit Noise(uint32_t seed) : state(seed ? seed : 1u) {}
  uint32_t next() {
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
  }
  unsigned char byte() { return (unsigned char)(next() & 0xFFu); }
};

GIMG_Raster * make_noise_gray8(uint32_t w, uint32_t h, uint32_t seed) {
  GIMG_Raster * r = make_raster(w, h, &GIMG_PIXEL_GRAY8);
  if (!r) {
    return nullptr;
  }
  Noise n(seed);
  for (uint32_t y = 0; y < h; y++) {
    for (uint32_t x = 0; x < w; x++) {
      *px8(r, x, y, 1) = n.byte();
    }
  }
  return r;
}

double stdev_gray8(const GIMG_Raster * r) {
  const uint32_t w = gimg_raster_width(r);
  const uint32_t h = gimg_raster_height(r);
  double sum = 0.0;
  for (uint32_t y = 0; y < h; y++) {
    for (uint32_t x = 0; x < w; x++) {
      sum += *cpx8(r, x, y, 1);
    }
  }
  const double mean = sum / (double)(w * h);
  double acc = 0.0;
  for (uint32_t y = 0; y < h; y++) {
    for (uint32_t x = 0; x < w; x++) {
      const double d = (double)*cpx8(r, x, y, 1) - mean;
      acc += d * d;
    }
  }
  return std::sqrt(acc / (double)(w * h));
}

GIMG_Result resize_with(const GIMG_Raster * src, uint32_t w, uint32_t h,
    GIMG_Resample_Filter f, GIMG_Raster ** out) {
  GIMG_Resize_Options o;
  gimg_resize_options_default(&o);
  o.filter = f;
  return gimg_ops_resize(src, w, h, &o, out);
}

GIMG_Result resize_linear(const GIMG_Raster * src, uint32_t w, uint32_t h,
    GIMG_Resample_Filter f, GIMG_Raster ** out) {
  GIMG_Resize_Options o;
  gimg_resize_options_default(&o);
  o.filter = f;
  o.space = GIMG_RESAMPLE_SPACE_LINEAR;
  return gimg_ops_resize(src, w, h, &o, out);
}

} // namespace

/**
 * A constant image resizes to the same constant, for every filter and at
 * every ratio.
 *
 * This is the kernel-normalization gate, and it is the one that hides: a
 * kernel whose weights sum to slightly more or less than one still produces a
 * picture, just a little brighter or darker than it should be, and no
 * eyeballed comparison finds it. Any ratio will do to catch it, so the sweep
 * is wide rather than deep.
 */
TEST(Resize, AConstantImageResizesToTheSameConstant) {
  const uint32_t sizes[][2] = {{64, 64}, {37, 23}, {8, 8}, {1, 9}, {100, 3}};
  const uint32_t targets[][2] = {
      {32, 32}, {128, 128}, {7, 61}, {1, 1}, {64, 64}, {3, 200}};
  for (GIMG_Resample_Filter f : kAllFilters) {
    for (const auto & s : sizes) {
      GIMG_Raster * src = make_raster(s[0], s[1], &GIMG_PIXEL_GRAY8);
      ASSERT_NE(src, nullptr);
      for (uint32_t y = 0; y < s[1]; y++) {
        for (uint32_t x = 0; x < s[0]; x++) {
          *px8(src, x, y, 1) = 173u;
        }
      }
      for (const auto & t : targets) {
        GIMG_Raster * dst = nullptr;
        ASSERT_EQ(resize_with(src, t[0], t[1], f, &dst), GIMG_OK)
            << filter_name(f);
        for (uint32_t y = 0; y < t[1]; y++) {
          for (uint32_t x = 0; x < t[0]; x++) {
            ASSERT_EQ(*cpx8(dst, x, y, 1), 173u)
                << filter_name(f) << " " << s[0] << "x" << s[1] << " -> "
                << t[0] << "x" << t[1] << " at " << x << "," << y;
          }
        }
        gimg_raster_destroy(dst);
      }
      gimg_raster_destroy(src);
    }
  }
}

/** Resizing to the size it already is changes nothing, for every filter. */
TEST(Resize, ResizingToTheSameSizeIsTheIdentity) {
  for (GIMG_Resample_Filter f : kAllFilters) {
    GIMG_Raster * src = make_noise_gray8(29, 17, 0x5EEDu);
    ASSERT_NE(src, nullptr);
    GIMG_Raster * dst = nullptr;
    ASSERT_EQ(resize_with(src, 29, 17, f, &dst), GIMG_OK) << filter_name(f);
    for (uint32_t y = 0; y < 17; y++) {
      for (uint32_t x = 0; x < 29; x++) {
        ASSERT_EQ(*cpx8(dst, x, y, 1), *cpx8(src, x, y, 1))
            << filter_name(f) << " at " << x << "," << y;
      }
    }
    gimg_raster_destroy(dst);
    gimg_raster_destroy(src);
  }
}

/** An integer enlargement with NEAREST repeats each sample exactly. */
TEST(Resize, NearestAtAnIntegerEnlargementRepeatsEachSample) {
  GIMG_Raster * src = make_noise_gray8(5, 4, 0xABCDu);
  ASSERT_NE(src, nullptr);
  for (uint32_t k = 2; k <= 4; k++) {
    GIMG_Raster * dst = nullptr;
    ASSERT_EQ(resize_with(src, 5 * k, 4 * k, GIMG_FILTER_NEAREST, &dst),
        GIMG_OK);
    for (uint32_t y = 0; y < 4 * k; y++) {
      for (uint32_t x = 0; x < 5 * k; x++) {
        ASSERT_EQ(*cpx8(dst, x, y, 1), *cpx8(src, x / k, y / k, 1))
            << "k=" << k << " at " << x << "," << y;
      }
    }
    gimg_raster_destroy(dst);
  }
  gimg_raster_destroy(src);
}

/**
 * Halving with BOX is the exact mean of each two-by-two block.
 *
 * This pins the support scaling to an answer that can be written down: if the
 * kernel is not stretched by the reduction ratio, BOX covers one source pixel
 * instead of four and this reads back a single sample rather than a mean.
 */
TEST(Resize, BoxHalvingIsTheExactMeanOfEachQuad) {
  GIMG_Raster * src = make_noise_gray8(16, 12, 0x1234u);
  ASSERT_NE(src, nullptr);
  GIMG_Raster * dst = nullptr;
  ASSERT_EQ(resize_with(src, 8, 6, GIMG_FILTER_BOX, &dst), GIMG_OK);
  for (uint32_t y = 0; y < 6; y++) {
    for (uint32_t x = 0; x < 8; x++) {
      const uint32_t sum = (uint32_t)*cpx8(src, 2 * x, 2 * y, 1) +
          *cpx8(src, 2 * x + 1, 2 * y, 1) + *cpx8(src, 2 * x, 2 * y + 1, 1) +
          *cpx8(src, 2 * x + 1, 2 * y + 1, 1);
      // The two passes each round once, so the result is the mean of two
      // rounded means rather than the mean of four samples; allow the one
      // count of slack that double rounding can introduce.
      const uint32_t expected = (sum + 2u) / 4u;
      const int got = *cpx8(dst, x, y, 1);
      ASSERT_LE(std::abs(got - (int)expected), 1)
          << "at " << x << "," << y << " expected " << expected << " got "
          << got;
    }
  }
  gimg_raster_destroy(dst);
  gimg_raster_destroy(src);
}

/**
 * NEAREST takes the source pixel that the destination pixel's centre falls
 * inside, with source pixel i covering [i, i+1).
 *
 * The expected index is computed here in exact integer arithmetic, so this
 * pins the convention rather than agreeing with whatever the implementation
 * happens to do in floating point. Every enlargement by a whole number agrees
 * under either convention, which is why the ratios here deliberately are not
 * whole numbers.
 *
 * **This is where this library and Pillow part company**, and only here: at a
 * destination centre that lands exactly on a source boundary, this takes the
 * pixel to the right, as the half-open interval says and as ImageMagick's
 * Point filter does. Pillow advances its source coordinate by repeated
 * addition, so it arrives a fraction below the boundary and takes the pixel to
 * the left. Across every pair of sizes from 1 to 59, that is the only case in
 * which the two disagree.
 */
TEST(Resize, NearestTakesThePixelItsCentreFallsIn) {
  const uint32_t pairs[][2] = {{2, 7}, {8, 7}, {7, 8}, {3, 5}, {5, 3},
      {256, 7}, {17, 40}, {40, 17}, {1, 6}, {6, 1}};
  for (const auto & p : pairs) {
    const uint32_t n = p[0];
    const uint32_t m = p[1];
    GIMG_Raster * src = make_raster(n, 1, &GIMG_PIXEL_GRAY8);
    ASSERT_NE(src, nullptr);
    for (uint32_t i = 0; i < n; i++) {
      *px8(src, i, 0, 1) = (unsigned char)(i % 251u);
    }
    GIMG_Raster * dst = nullptr;
    ASSERT_EQ(resize_with(src, m, 1, GIMG_FILTER_NEAREST, &dst), GIMG_OK);
    for (uint32_t x = 0; x < m; x++) {
      // floor((x + 0.5) * n / m), with no floating point anywhere.
      uint64_t sx = ((uint64_t)(2u * x + 1u) * (uint64_t)n) / (2ull * m);
      if (sx >= n) {
        sx = n - 1u;
      }
      ASSERT_EQ(*cpx8(dst, x, 0, 1), (unsigned char)(sx % 251u))
          << n << " -> " << m << " at " << x;
    }
    gimg_raster_destroy(dst);
    gimg_raster_destroy(src);

    // The same, down the other axis. The two are separate lines of
    // arithmetic, and a half-pixel dropped from only one of them leaves a
    // width-wise test entirely happy.
    GIMG_Raster * column = make_raster(1, n, &GIMG_PIXEL_GRAY8);
    ASSERT_NE(column, nullptr);
    for (uint32_t i = 0; i < n; i++) {
      *px8(column, 0, i, 1) = (unsigned char)(i % 251u);
    }
    GIMG_Raster * tall = nullptr;
    ASSERT_EQ(resize_with(column, 1, m, GIMG_FILTER_NEAREST, &tall), GIMG_OK);
    for (uint32_t y = 0; y < m; y++) {
      uint64_t sy = ((uint64_t)(2u * y + 1u) * (uint64_t)n) / (2ull * m);
      if (sy >= n) {
        sy = n - 1u;
      }
      ASSERT_EQ(*cpx8(tall, 0, y, 1), (unsigned char)(sy % 251u))
          << n << " -> " << m << " down, at " << y;
    }
    gimg_raster_destroy(tall);
    gimg_raster_destroy(column);
  }
}

/**
 * A uniform translucent colour survives the round trip through
 * premultiplication unchanged.
 *
 * Premultiplying on the way in and forgetting to divide back out on the way
 * out leaves every colour scaled by its own alpha - a half-transparent red
 * comes back as a half-transparent dark red. Nothing about the *shape* of the
 * picture changes, so the halo test cannot see it: there the channels stay
 * equal to one another, just all wrong together.
 */
TEST(Resize, ATranslucentColourComesBackUnchanged) {
  // At alpha 128, rounding each of the two multiplications to nearest keeps
  // every one of the 256 possible channel values within one count. Truncating
  // instead doubles that, and errs by two or more for 126 of them - 5, 13 and
  // 21 among the smallest. Those are here so that the rounding mode is pinned
  // and not merely covered by slack.
  const unsigned char colours[][3] = {
      {240u, 30u, 90u}, {5u, 13u, 21u}, {3u, 250u, 127u}};
  for (const auto & colour : colours) {
    GIMG_Raster * src = make_raster(16, 16, &GIMG_PIXEL_RGBA8);
    ASSERT_NE(src, nullptr);
    for (uint32_t y = 0; y < 16; y++) {
      for (uint32_t x = 0; x < 16; x++) {
        unsigned char * p = px8(src, x, y, 4);
        p[0] = colour[0];
        p[1] = colour[1];
        p[2] = colour[2];
        p[3] = 128u;
      }
    }
    for (GIMG_Resample_Filter f : kAllFilters) {
      for (uint32_t size : {8u, 16u, 33u}) {
        GIMG_Raster * dst = nullptr;
        ASSERT_EQ(resize_with(src, size, size, f, &dst), GIMG_OK);
        for (uint32_t y = 0; y < size; y++) {
          for (uint32_t x = 0; x < size; x++) {
            const unsigned char * p = cpx8(dst, x, y, 4);
            ASSERT_EQ(p[3], 128u) << filter_name(f);
            for (uint8_t c = 0; c < 3u; c++) {
              ASSERT_LE(std::abs((int)p[c] - (int)colour[c]), 1)
                  << filter_name(f) << " channel " << (int)c << " at " << x
                  << "," << y << ": expected about " << (int)colour[c]
                  << " got " << (int)p[c];
            }
          }
        }
        gimg_raster_destroy(dst);
      }
    }
    gimg_raster_destroy(src);
  }
}

/**
 * A reduction must actually low-pass.
 *
 * Incompressible noise has a standard deviation near 74; a true average over
 * an n-by-n region divides that by n, while a filter that samples instead of
 * averaging leaves it where it was. Run at a mild ratio as well as a large
 * one, because a kernel whose support is not stretched still looks right at
 * eight times and aliases at a ratio near one - which is exactly the mistake
 * the first draft of the AUTO rule made.
 */
TEST(Resize, AReductionActuallyLowPasses) {
  GIMG_Raster * src = make_noise_gray8(256, 256, 0x9E3779B9u);
  ASSERT_NE(src, nullptr);
  const double source_sd = stdev_gray8(src);
  ASSERT_GT(source_sd, 65.0) << "the noise source is not noisy";

  for (GIMG_Resample_Filter f : kAveragingFilters) {
    GIMG_Raster * big = nullptr;
    ASSERT_EQ(resize_with(src, 32, 32, f, &big), GIMG_OK);
    // A true 8x8 mean would give about 74/8 = 9.2. Anything still above a
    // third of the source's spread is sampling, not averaging.
    EXPECT_LT(stdev_gray8(big), source_sd / 3.0)
        << filter_name(f) << " at 8x reduction";
    gimg_raster_destroy(big);

    if (f == GIMG_FILTER_BOX) {
      // BOX is documented as behaving like NEAREST near 1:1 - its support is
      // half a pixel, and stretched by a ratio near one it still covers a
      // single sample. That is why AUTO does not select it.
      continue;
    }
    GIMG_Raster * mild = nullptr;
    ASSERT_EQ(resize_with(src, 250, 250, f, &mild), GIMG_OK);
    EXPECT_LT(stdev_gray8(mild), source_sd)
        << filter_name(f) << " does not filter at all at a mild reduction";
    gimg_raster_destroy(mild);
  }
  gimg_raster_destroy(src);
}

/**
 * AUTO is pinned to the filter it aliases.
 *
 * If somebody changes what it maps to, this fails, and the change has to be
 * deliberate rather than a quiet difference in every caller's output.
 */
TEST(Resize, AutoIsPinnedToCatmullRom) {
  GIMG_Raster * src = make_noise_gray8(40, 30, 0x0FFEEu);
  ASSERT_NE(src, nullptr);
  const uint32_t targets[][2] = {{20, 15}, {80, 60}, {13, 41}};
  for (const auto & t : targets) {
    GIMG_Raster * a = nullptr;
    GIMG_Raster * b = nullptr;
    ASSERT_EQ(resize_with(src, t[0], t[1], GIMG_FILTER_AUTO, &a), GIMG_OK);
    ASSERT_EQ(resize_with(src, t[0], t[1], GIMG_FILTER_CATMULL_ROM, &b),
        GIMG_OK);
    EXPECT_TRUE(gimg_ops_raster_equal(a, b))
        << "AUTO no longer matches CATMULL_ROM at " << t[0] << "x" << t[1];
    gimg_raster_destroy(a);
    gimg_raster_destroy(b);
  }
  gimg_raster_destroy(src);
}

/** An opaque image stays opaque; nothing leaks out of the alpha channel. */
TEST(Resize, AnOpaqueImageStaysOpaque) {
  GIMG_Raster * src = make_raster(24, 24, &GIMG_PIXEL_RGBA8);
  ASSERT_NE(src, nullptr);
  Noise n(0x77u);
  for (uint32_t y = 0; y < 24; y++) {
    for (uint32_t x = 0; x < 24; x++) {
      unsigned char * p = px8(src, x, y, 4);
      p[0] = n.byte();
      p[1] = n.byte();
      p[2] = n.byte();
      p[3] = 255u;
    }
  }
  for (GIMG_Resample_Filter f : kAllFilters) {
    for (uint32_t size : {7u, 24u, 61u}) {
      GIMG_Raster * dst = nullptr;
      ASSERT_EQ(resize_with(src, size, size, f, &dst), GIMG_OK);
      for (uint32_t y = 0; y < size; y++) {
        for (uint32_t x = 0; x < size; x++) {
          ASSERT_EQ(cpx8(dst, x, y, 4)[3], 255u)
              << filter_name(f) << " at " << x << "," << y;
        }
      }
      gimg_raster_destroy(dst);
    }
  }
  gimg_raster_destroy(src);
}

/**
 * A fully transparent region does not tint its neighbours.
 *
 * This is the premultiplication gate. The source is transparent *red* beside
 * opaque white, which is the arrangement where the failure is loudest:
 * averaging straight alpha pulls the green and blue of the white pixels down
 * towards the red's zero while leaving red alone, so the edge comes out pink
 * and the three channels stop agreeing. Premultiplied, the transparent pixels
 * contribute nothing at all and white stays white.
 */
TEST(Resize, TransparencyDoesNotTintItsNeighbours) {
  const uint32_t w = 32;
  const uint32_t h = 8;
  GIMG_Raster * src = make_raster(w, h, &GIMG_PIXEL_RGBA8);
  ASSERT_NE(src, nullptr);
  for (uint32_t y = 0; y < h; y++) {
    for (uint32_t x = 0; x < w; x++) {
      unsigned char * p = px8(src, x, y, 4);
      if (x < w / 2) {
        p[0] = 255u; // red, and wholly invisible
        p[1] = 0u;
        p[2] = 0u;
        p[3] = 0u;
      }
      else {
        p[0] = 255u;
        p[1] = 255u;
        p[2] = 255u;
        p[3] = 255u;
      }
    }
  }
  for (GIMG_Resample_Filter f : kAveragingFilters) {
    GIMG_Raster * dst = nullptr;
    ASSERT_EQ(resize_with(src, w / 2, h / 2, f, &dst), GIMG_OK);
    for (uint32_t y = 0; y < h / 2; y++) {
      for (uint32_t x = 0; x < w / 2; x++) {
        const unsigned char * p = cpx8(dst, x, y, 4);
        if (p[3] == 0u) {
          continue; // nothing is shown here; its colour says nothing
        }
        EXPECT_EQ(p[0], p[1])
            << filter_name(f) << ": red and green disagree at " << x << ","
            << y << " - the transparent red bled in";
        EXPECT_EQ(p[1], p[2]) << filter_name(f) << " at " << x << "," << y;
      }
    }
    gimg_raster_destroy(dst);
  }
  gimg_raster_destroy(src);
}

/**
 * Each row of a width-only resize is the resize of that row on its own.
 *
 * The passes are separable by construction, so this is really a check that
 * the indexing does not wander between rows - a stride confused for a row
 * length shows up here and almost nowhere else.
 */
TEST(Resize, AWidthOnlyResizeTreatsEachRowIndependently) {
  const uint32_t w = 23;
  const uint32_t h = 9;
  GIMG_Raster * src = make_noise_gray8(w, h, 0xC0FFEEu);
  ASSERT_NE(src, nullptr);
  for (GIMG_Resample_Filter f : kAllFilters) {
    GIMG_Raster * wide = nullptr;
    ASSERT_EQ(resize_with(src, 11, h, f, &wide), GIMG_OK);
    for (uint32_t y = 0; y < h; y++) {
      GIMG_Raster * row = make_raster(w, 1, &GIMG_PIXEL_GRAY8);
      ASSERT_NE(row, nullptr);
      for (uint32_t x = 0; x < w; x++) {
        *px8(row, x, 0, 1) = *cpx8(src, x, y, 1);
      }
      GIMG_Raster * row_out = nullptr;
      ASSERT_EQ(resize_with(row, 11, 1, f, &row_out), GIMG_OK);
      for (uint32_t x = 0; x < 11; x++) {
        ASSERT_EQ(*cpx8(wide, x, y, 1), *cpx8(row_out, x, 0, 1))
            << filter_name(f) << " row " << y << " at " << x;
      }
      gimg_raster_destroy(row_out);
      gimg_raster_destroy(row);
    }
    gimg_raster_destroy(wide);
  }
  gimg_raster_destroy(src);
}

/** The extreme shapes: one pixel in, one pixel out, and single-pixel axes. */
TEST(Resize, TheExtremeShapesAreHandled) {
  for (GIMG_Resample_Filter f : kAllFilters) {
    GIMG_Raster * one = make_raster(1, 1, &GIMG_PIXEL_GRAY8);
    ASSERT_NE(one, nullptr);
    *px8(one, 0, 0, 1) = 200u;
    GIMG_Raster * big = nullptr;
    ASSERT_EQ(resize_with(one, 9, 9, f, &big), GIMG_OK) << filter_name(f);
    for (uint32_t y = 0; y < 9; y++) {
      for (uint32_t x = 0; x < 9; x++) {
        ASSERT_EQ(*cpx8(big, x, y, 1), 200u) << filter_name(f);
      }
    }
    gimg_raster_destroy(big);
    gimg_raster_destroy(one);

    GIMG_Raster * strip = make_noise_gray8(64, 1, 0x31337u);
    ASSERT_NE(strip, nullptr);
    GIMG_Raster * small = nullptr;
    ASSERT_EQ(resize_with(strip, 1, 1, f, &small), GIMG_OK) << filter_name(f);
    EXPECT_EQ(gimg_raster_width(small), 1u);
    gimg_raster_destroy(small);
    GIMG_Raster * tall = nullptr;
    ASSERT_EQ(resize_with(strip, 3, 40, f, &tall), GIMG_OK) << filter_name(f);
    EXPECT_EQ(gimg_raster_height(tall), 40u);
    gimg_raster_destroy(tall);
    gimg_raster_destroy(strip);
  }
}

/** Sixteen-bit samples go through the same path and keep their range. */
TEST(Resize, SixteenBitSamplesKeepTheirRange) {
  GIMG_Raster * src = make_raster(20, 20, &GIMG_PIXEL_GRAY16);
  ASSERT_NE(src, nullptr);
  for (uint32_t y = 0; y < 20; y++) {
    for (uint32_t x = 0; x < 20; x++) {
      ((uint16_t *)px8(src, x, y, 2))[0] = 65535u;
    }
  }
  for (GIMG_Resample_Filter f : kAllFilters) {
    GIMG_Raster * dst = nullptr;
    ASSERT_EQ(resize_with(src, 9, 33, f, &dst), GIMG_OK) << filter_name(f);
    for (uint32_t y = 0; y < 33; y++) {
      for (uint32_t x = 0; x < 9; x++) {
        ASSERT_EQ(((const uint16_t *)cpx8(dst, x, y, 2))[0], 65535u)
            << filter_name(f) << " at " << x << "," << y;
      }
    }
    gimg_raster_destroy(dst);
  }
  gimg_raster_destroy(src);
}


/**
 * Averaging in linear light gives a different, lighter answer than averaging
 * the encoded values - and this is the case that shows why the option exists.
 *
 * Reduce a black-and-white checkerboard to a single pixel. Half the light is
 * present, so the honest answer is the encoding of 0.5, which sRGB puts near
 * 188. Averaging the encoded values instead gives 128, which is the encoding
 * of about 21% of the light: the picture comes out much darker than the
 * scene. Every library that resamples in the encoded space has this, which is
 * why it is the default here - it is what the oracles do - and why the other
 * setting is offered at all.
 */
TEST(Resize, LinearLightAveragesTheLightRatherThanTheEncoding) {
  GIMG_Raster * src = make_raster(64, 64, &GIMG_PIXEL_GRAY8);
  ASSERT_NE(src, nullptr);
  for (uint32_t y = 0; y < 64; y++) {
    for (uint32_t x = 0; x < 64; x++) {
      *px8(src, x, y, 1) = ((x + y) & 1u) ? 255u : 0u;
    }
  }
  GIMG_Raster * encoded = nullptr;
  GIMG_Raster * linear = nullptr;
  ASSERT_EQ(resize_with(src, 1, 1, GIMG_FILTER_BOX, &encoded), GIMG_OK);
  ASSERT_EQ(resize_linear(src, 1, 1, GIMG_FILTER_BOX, &linear), GIMG_OK);

  const int got_encoded = *cpx8(encoded, 0, 0, 1);
  const int got_linear = *cpx8(linear, 0, 0, 1);
  EXPECT_GE(got_encoded, 126);
  EXPECT_LE(got_encoded, 129) << "the encoded average should be about 128";
  EXPECT_GE(got_linear, 185);
  EXPECT_LE(got_linear, 192)
      << "half the light encodes to about 188 in sRGB; got " << got_linear;

  gimg_raster_destroy(encoded);
  gimg_raster_destroy(linear);
  gimg_raster_destroy(src);
}

/**
 * Linearizing and re-encoding is exactly reversible, so a resize that changes
 * no dimension changes no sample.
 *
 * This is what the reverse table is built by walking the forward one for. The
 * analytic inverse rounds on its own and loses a count here and there in the
 * darks - where sRGB is steepest - and a caller who resized to the size they
 * already had would find the picture very slightly altered.
 */
TEST(Resize, LinearLightAtTheSameSizeIsStillTheIdentity) {
  for (GIMG_Resample_Filter f : kAveragingFilters) {
    GIMG_Raster * src = make_raster(256, 1, &GIMG_PIXEL_GRAY8);
    ASSERT_NE(src, nullptr);
    for (uint32_t v = 0; v < 256; v++) {
      *px8(src, v, 0, 1) = (unsigned char)v;
    }
    GIMG_Raster * dst = nullptr;
    ASSERT_EQ(resize_linear(src, 256, 1, f, &dst), GIMG_OK) << filter_name(f);
    for (uint32_t v = 0; v < 256; v++) {
      ASSERT_EQ(*cpx8(dst, v, 0, 1), (unsigned char)v)
          << filter_name(f) << ": sample " << v
          << " did not survive the round trip through linear light";
    }
    gimg_raster_destroy(dst);
    gimg_raster_destroy(src);
  }
}

/**
 * Alpha is left alone by the transfer.
 *
 * It is a coverage fraction, not a light level; there is nothing non-linear
 * about it to undo. Running it through sRGB's curve would make a half-covered
 * pixel report about three quarters coverage, which is both wrong and
 * invisible in any picture that is either wholly opaque or wholly clear.
 */
TEST(Resize, LinearLightLeavesAlphaAlone) {
  GIMG_Raster * src = make_raster(16, 16, &GIMG_PIXEL_RGBA8);
  ASSERT_NE(src, nullptr);
  for (uint32_t y = 0; y < 16; y++) {
    for (uint32_t x = 0; x < 16; x++) {
      unsigned char * p = px8(src, x, y, 4);
      p[0] = 200u;
      p[1] = 100u;
      p[2] = 50u;
      p[3] = 128u; // half covered, and it must stay half covered
    }
  }
  for (GIMG_Resample_Filter f : kAveragingFilters) {
    for (uint32_t size : {8u, 16u, 40u}) {
      GIMG_Raster * dst = nullptr;
      ASSERT_EQ(resize_linear(src, size, size, f, &dst), GIMG_OK);
      for (uint32_t y = 0; y < size; y++) {
        for (uint32_t x = 0; x < size; x++) {
          ASSERT_EQ(cpx8(dst, x, y, 4)[3], 128u)
              << filter_name(f) << " at " << x << "," << y
              << ": alpha went through the transfer function";
        }
      }
      gimg_raster_destroy(dst);
    }
  }
  gimg_raster_destroy(src);
}

/** A constant image is still constant when the averaging is done in light. */
TEST(Resize, LinearLightKeepsAConstantConstant) {
  for (unsigned char value : {0u, 1u, 17u, 128u, 254u, 255u}) {
    GIMG_Raster * src = make_raster(40, 24, &GIMG_PIXEL_GRAY8);
    ASSERT_NE(src, nullptr);
    for (uint32_t y = 0; y < 24; y++) {
      for (uint32_t x = 0; x < 40; x++) {
        *px8(src, x, y, 1) = value;
      }
    }
    for (GIMG_Resample_Filter f : kAveragingFilters) {
      GIMG_Raster * dst = nullptr;
      ASSERT_EQ(resize_linear(src, 13, 61, f, &dst), GIMG_OK);
      for (uint32_t y = 0; y < 61; y++) {
        for (uint32_t x = 0; x < 13; x++) {
          ASSERT_EQ(*cpx8(dst, x, y, 1), value)
              << filter_name(f) << " value " << (int)value;
        }
      }
      gimg_raster_destroy(dst);
    }
    gimg_raster_destroy(src);
  }
}

/**
 * NEAREST returns a sample rather than an average, so there is nothing for the
 * colour space to change. Asking for linear light gets the same bytes, which
 * is worth pinning: the alternative would be a path that quietly linearizes
 * and re-encodes a value for no reason and loses a count doing it.
 */
TEST(Resize, LinearLightMakesNoDifferenceToNearest) {
  GIMG_Raster * src = make_noise_gray8(33, 21, 0xBEEFu);
  ASSERT_NE(src, nullptr);
  GIMG_Raster * a = nullptr;
  GIMG_Raster * b = nullptr;
  ASSERT_EQ(resize_with(src, 17, 44, GIMG_FILTER_NEAREST, &a), GIMG_OK);
  ASSERT_EQ(resize_linear(src, 17, 44, GIMG_FILTER_NEAREST, &b), GIMG_OK);
  EXPECT_TRUE(gimg_ops_raster_equal(a, b));
  gimg_raster_destroy(a);
  gimg_raster_destroy(b);
  gimg_raster_destroy(src);
}

TEST(Resize, WhatIsRefused) {
  GIMG_Raster * src = make_noise_gray8(8, 8, 1u);
  ASSERT_NE(src, nullptr);
  GIMG_Raster * dst = (GIMG_Raster *)0x1;

  EXPECT_EQ(gimg_ops_resize(nullptr, 4, 4, nullptr, &dst), GIMG_ERR_INTERNAL);
  EXPECT_EQ(gimg_ops_resize(src, 4, 4, nullptr, nullptr), GIMG_ERR_INTERNAL);

  dst = (GIMG_Raster *)0x1;
  EXPECT_EQ(gimg_ops_resize(src, 0, 4, nullptr, &dst), GIMG_ERR_INTERNAL);
  EXPECT_EQ(dst, nullptr);
  dst = (GIMG_Raster *)0x1;
  EXPECT_EQ(gimg_ops_resize(src, 4, 0, nullptr, &dst), GIMG_ERR_INTERNAL);
  EXPECT_EQ(dst, nullptr);

  GIMG_Resize_Options o;
  gimg_resize_options_default(&o);
  o.filter = GIMG_FILTER_COUNT;
  dst = nullptr;
  EXPECT_EQ(gimg_ops_resize(src, 4, 4, &o, &dst), GIMG_ERR_UNSUPPORTED);

  gimg_resize_options_default(&o);
  o.space = (GIMG_Resample_Space)77;
  dst = nullptr;
  EXPECT_EQ(gimg_ops_resize(src, 4, 4, &o, &dst), GIMG_ERR_UNSUPPORTED);

  // Linear light applies sRGB's curve, which says something about light. Ink
  // amounts and unnamed channels are not light, so they are refused rather
  // than run through a curve that means nothing for them.
  GIMG_Raster * cmyk = nullptr;
  ASSERT_EQ(gimg_raster_create(8, 8, &GIMG_PIXEL_CMYK8, GIMG_RASTER_OWNED,
                nullptr, 0, &cmyk),
      GIMG_OK);
  dst = nullptr;
  EXPECT_EQ(resize_linear(cmyk, 4, 4, GIMG_FILTER_BOX, &dst),
      GIMG_ERR_UNSUPPORTED);
  // ...but the same raster resizes perfectly well in the encoded space.
  dst = nullptr;
  EXPECT_EQ(resize_with(cmyk, 4, 4, GIMG_FILTER_BOX, &dst), GIMG_OK);
  gimg_raster_destroy(dst);
  gimg_raster_destroy(cmyk);

  // An indexed raster: the average of two palette indices is not an index.
  GIMG_Pixel_Format indexed;
  memset(&indexed, 0, sizeof(indexed));
  indexed.channel_model = GIMG_CHANNEL_INDEXED;
  indexed.channel_type = GIMG_CHANNEL_UINT;
  indexed.layout = GIMG_LAYOUT_INTERLEAVED;
  indexed.channel_count = 1;
  indexed.bits_per_channel[0] = 8;
  GIMG_Raster * pal = nullptr;
  ASSERT_EQ(gimg_raster_create(
                8, 8, &indexed, GIMG_RASTER_OWNED, nullptr, 0, &pal),
      GIMG_OK);
  dst = nullptr;
  EXPECT_EQ(gimg_ops_resize(pal, 4, 4, nullptr, &dst), GIMG_ERR_UNSUPPORTED);
  gimg_raster_destroy(pal);

  gimg_raster_destroy(src);
}

TEST(Resize, TheColourDescriptionSurvives) {
  GIMG_Raster * src = make_noise_gray8(10, 10, 5u);
  ASSERT_NE(src, nullptr);
  const unsigned char profile[] = {0x01, 0x02, 0x03, 0x04};
  GIMG_Color_Info ci;
  gimg_color_info_default(&ci);
  ci.transfer = GIMG_TRANSFER_SRGB;
  ci.intent = GIMG_INTENT_SATURATION;
  ci.icc_bytes = profile;
  ci.icc_size = sizeof(profile);
  ASSERT_EQ(gimg_raster_set_color_info(src, &ci), GIMG_OK);
  for (GIMG_Resample_Filter f : kAllFilters) {
    GIMG_Raster * dst = nullptr;
    ASSERT_EQ(resize_with(src, 5, 20, f, &dst), GIMG_OK);
    const GIMG_Color_Info * got = gimg_raster_color_info_const(dst);
    ASSERT_NE(got, nullptr);
    EXPECT_EQ(got->transfer, GIMG_TRANSFER_SRGB) << filter_name(f);
    EXPECT_EQ(got->intent, GIMG_INTENT_SATURATION) << filter_name(f);
    ASSERT_EQ(got->icc_size, sizeof(profile));
    EXPECT_EQ(memcmp(got->icc_bytes, profile, sizeof(profile)), 0);
    gimg_raster_destroy(dst);
  }
  gimg_raster_destroy(src);
}

/** The defaults are memset, so a field added later is not left uninitialised. */
TEST(Resize, TheDefaultsAreZeroedFirst) {
  GIMG_Resize_Options o;
  memset(&o, 0xA5, sizeof(o));
  gimg_resize_options_default(&o);
  EXPECT_EQ(o.filter, GIMG_FILTER_AUTO);
  EXPECT_EQ(o.space, GIMG_RESAMPLE_SPACE_ENCODED);
  for (size_t i = 0; i < sizeof(o._reserved); i++) {
    EXPECT_EQ(o._reserved[i], 0u) << "reserved byte " << i;
  }
  // A zeroed struct and the defaults must mean the same thing, because
  // `GIMG_Resize_Options o = {0};` is what a caller writes.
  GIMG_Resize_Options zeroed;
  memset(&zeroed, 0, sizeof(zeroed));
  EXPECT_EQ(memcmp(&o, &zeroed, sizeof(o)), 0);
}

int main(int argc, char ** argv) {
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}

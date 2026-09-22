/**
 * @file
 *
 * Unit tests for the geometry operations: cropping.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <ghoti.io/image/color.h>
#include <ghoti.io/image/meta.h>
#include <ghoti.io/image/ops.h>
#include <ghoti.io/image/raster.h>
#include <cstring>
#include <gtest/gtest.h>
#include <vector>

namespace {

/**
 * A raster whose every pixel says where it is, so that a crop which takes the
 * wrong rectangle - or reads across the stride padding - produces values that
 * name the place it went wrong rather than something merely unequal.
 */
GIMG_Raster * make_positional_rgba8(uint32_t w, uint32_t h) {
  GIMG_Raster * r = nullptr;
  if (gimg_raster_create(w, h, &GIMG_PIXEL_RGBA8, GIMG_RASTER_OWNED, nullptr, 0,
          &r) != GIMG_OK) {
    return nullptr;
  }
  unsigned char * p = (unsigned char *)gimg_raster_pixels(r);
  size_t stride = gimg_raster_stride_bytes(r);
  for (uint32_t y = 0; y < h; y++) {
    for (uint32_t x = 0; x < w; x++) {
      unsigned char * px = p + (size_t)y * stride + (size_t)x * 4u;
      px[0] = (unsigned char)x;
      px[1] = (unsigned char)y;
      px[2] = (unsigned char)(x * 7u + y * 13u);
      px[3] = 255u;
    }
  }
  return r;
}

void expect_pixel_is(const GIMG_Raster * r, uint32_t x, uint32_t y,
    uint32_t src_x, uint32_t src_y) {
  const unsigned char * p = (const unsigned char *)gimg_raster_pixels_const(r);
  size_t stride = gimg_raster_stride_bytes(r);
  const unsigned char * px = p + (size_t)y * stride + (size_t)x * 4u;
  EXPECT_EQ(px[0], (unsigned char)src_x) << "at " << x << "," << y;
  EXPECT_EQ(px[1], (unsigned char)src_y) << "at " << x << "," << y;
  EXPECT_EQ(px[2], (unsigned char)(src_x * 7u + src_y * 13u))
      << "at " << x << "," << y;
}

} // namespace

TEST(Crop, TakesTheRectangleAsked) {
  GIMG_Raster * src = make_positional_rgba8(16, 12);
  ASSERT_NE(src, nullptr);
  GIMG_Raster * dst = nullptr;
  ASSERT_EQ(gimg_ops_crop(src, 3, 5, 4, 2, &dst), GIMG_OK);
  ASSERT_NE(dst, nullptr);
  EXPECT_EQ(gimg_raster_width(dst), 4u);
  EXPECT_EQ(gimg_raster_height(dst), 2u);
  for (uint32_t y = 0; y < 2; y++) {
    for (uint32_t x = 0; x < 4; x++) {
      expect_pixel_is(dst, x, y, 3u + x, 5u + y);
    }
  }
  gimg_raster_destroy(dst);
  gimg_raster_destroy(src);
}

/**
 * The whole rectangle is the identity.  This is the cheapest check that the
 * row arithmetic does not drift: an off-by-one in the stride or the origin
 * shows up here before any smaller rectangle is tried.
 */
TEST(Crop, TheWholeRectangleIsTheIdentity) {
  GIMG_Raster * src = make_positional_rgba8(9, 7);
  ASSERT_NE(src, nullptr);
  GIMG_Raster * dst = nullptr;
  ASSERT_EQ(gimg_ops_crop(src, 0, 0, 9, 7, &dst), GIMG_OK);
  ASSERT_NE(dst, nullptr);
  EXPECT_EQ(gimg_raster_width(dst), 9u);
  EXPECT_EQ(gimg_raster_height(dst), 7u);
  for (uint32_t y = 0; y < 7; y++) {
    for (uint32_t x = 0; x < 9; x++) {
      expect_pixel_is(dst, x, y, x, y);
    }
  }
  gimg_raster_destroy(dst);
  gimg_raster_destroy(src);
}

/** Cropping twice reaches the same place as cropping once. */
TEST(Crop, TwoCropsComposeIntoOne) {
  GIMG_Raster * src = make_positional_rgba8(20, 20);
  ASSERT_NE(src, nullptr);
  GIMG_Raster * once = nullptr;
  ASSERT_EQ(gimg_ops_crop(src, 6, 9, 3, 4, &once), GIMG_OK);

  GIMG_Raster * step = nullptr;
  ASSERT_EQ(gimg_ops_crop(src, 4, 7, 10, 10, &step), GIMG_OK);
  GIMG_Raster * twice = nullptr;
  ASSERT_EQ(gimg_ops_crop(step, 2, 2, 3, 4, &twice), GIMG_OK);

  ASSERT_EQ(gimg_raster_width(once), gimg_raster_width(twice));
  ASSERT_EQ(gimg_raster_height(once), gimg_raster_height(twice));
  EXPECT_TRUE(gimg_ops_raster_equal(once, twice));

  gimg_raster_destroy(twice);
  gimg_raster_destroy(step);
  gimg_raster_destroy(once);
  gimg_raster_destroy(src);
}

/**
 * A one-pixel crop from each corner.  The corners are where an origin
 * computed with the wrong sign, or a bound tested with the wrong comparison,
 * still lands inside the buffer and returns plausible bytes.
 */
TEST(Crop, EveryCornerIsReachable) {
  GIMG_Raster * src = make_positional_rgba8(11, 6);
  ASSERT_NE(src, nullptr);
  const uint32_t corners[4][2] = {{0, 0}, {10, 0}, {0, 5}, {10, 5}};
  for (const auto & c : corners) {
    GIMG_Raster * dst = nullptr;
    ASSERT_EQ(gimg_ops_crop(src, c[0], c[1], 1, 1, &dst), GIMG_OK)
        << "corner " << c[0] << "," << c[1];
    ASSERT_NE(dst, nullptr);
    expect_pixel_is(dst, 0, 0, c[0], c[1]);
    gimg_raster_destroy(dst);
  }
  gimg_raster_destroy(src);
}

/**
 * A rectangle that leaves the source is refused, and the out pointer is
 * cleared rather than left holding whatever the caller had in it.
 *
 * The x + width cases are the ones that matter: written as an addition the
 * bound wraps, and a rectangle starting near UINT32_MAX tests as if it were
 * inside the image.
 */
TEST(Crop, ARectangleOutsideTheImageIsRefused) {
  GIMG_Raster * src = make_positional_rgba8(8, 8);
  ASSERT_NE(src, nullptr);
  struct Case {
    uint32_t x, y, w, h;
    const char * why;
  };
  const Case cases[] = {
      {0, 0, 9, 8, "wider than the source"},
      {0, 0, 8, 9, "taller than the source"},
      {1, 0, 8, 8, "runs off the right edge by one"},
      {0, 1, 8, 8, "runs off the bottom edge by one"},
      {8, 0, 1, 1, "starts one past the right edge"},
      {0, 8, 1, 1, "starts one past the bottom edge"},
      {0, 0, 0, 4, "zero width"},
      {0, 0, 4, 0, "zero height"},
      {UINT32_MAX - 1u, 0, 4, 1, "x + width wraps"},
      {0, UINT32_MAX - 1u, 1, 4, "y + height wraps"},
  };
  for (const auto & c : cases) {
    GIMG_Raster * dst = (GIMG_Raster *)0x1;
    EXPECT_EQ(gimg_ops_crop(src, c.x, c.y, c.w, c.h, &dst), GIMG_ERR_INTERNAL)
        << c.why;
    EXPECT_EQ(dst, nullptr) << c.why;
  }
  gimg_raster_destroy(src);
}

TEST(Crop, NullArgumentsAreRefused) {
  GIMG_Raster * src = make_positional_rgba8(4, 4);
  ASSERT_NE(src, nullptr);
  GIMG_Raster * dst = nullptr;
  EXPECT_EQ(gimg_ops_crop(nullptr, 0, 0, 1, 1, &dst), GIMG_ERR_INTERNAL);
  EXPECT_EQ(gimg_ops_crop(src, 0, 0, 1, 1, nullptr), GIMG_ERR_INTERNAL);
  gimg_raster_destroy(src);
}

/**
 * The colour description survives, profile included.  Showing less of a
 * picture does not change what its samples mean, and the same omission in the
 * bit-depth and format conversions once sent a tagged 16-bit PNG out of a
 * JPEG save untagged.
 */
TEST(Crop, TheColourDescriptionSurvives) {
  GIMG_Raster * src = make_positional_rgba8(6, 6);
  ASSERT_NE(src, nullptr);
  const unsigned char profile[] = {0xDE, 0xAD, 0xBE, 0xEF, 0x01, 0x02};
  GIMG_Color_Info ci;
  gimg_color_info_default(&ci);
  ci.transfer = GIMG_TRANSFER_SRGB;
  ci.intent = GIMG_INTENT_SATURATION;
  ci.icc_bytes = profile;
  ci.icc_size = sizeof(profile);
  ASSERT_EQ(gimg_raster_set_color_info(src, &ci), GIMG_OK);

  GIMG_Raster * dst = nullptr;
  ASSERT_EQ(gimg_ops_crop(src, 1, 1, 2, 2, &dst), GIMG_OK);
  const GIMG_Color_Info * got = gimg_raster_color_info_const(dst);
  ASSERT_NE(got, nullptr);
  EXPECT_EQ(got->transfer, GIMG_TRANSFER_SRGB);
  EXPECT_EQ(got->intent, GIMG_INTENT_SATURATION);
  ASSERT_EQ(got->icc_size, sizeof(profile));
  ASSERT_NE(got->icc_bytes, nullptr);
  EXPECT_EQ(memcmp(got->icc_bytes, profile, sizeof(profile)), 0);
  // Deep-copied, so destroying the source leaves the crop's profile valid.
  EXPECT_NE(got->icc_bytes, profile);

  gimg_raster_destroy(dst);
  gimg_raster_destroy(src);
}

/**
 * A crop reads whole pixels and never interprets one, so it works for formats
 * the resampler refuses.  16-bit and CMYK are the two that would break a
 * byte-per-sample assumption in opposite ways.
 */
TEST(Crop, WorksForFormatsTheResamplerWillNotTake) {
  const GIMG_Pixel_Format * formats[] = {
      &GIMG_PIXEL_GRAY16, &GIMG_PIXEL_RGBA16, &GIMG_PIXEL_CMYK8};
  for (const GIMG_Pixel_Format * fmt : formats) {
    GIMG_Raster * src = nullptr;
    ASSERT_EQ(gimg_raster_create(
                  8, 4, fmt, GIMG_RASTER_OWNED, nullptr, 0, &src),
        GIMG_OK);
    size_t bpp = gimg_raster_bytes_per_pixel(fmt);
    unsigned char * p = (unsigned char *)gimg_raster_pixels(src);
    size_t stride = gimg_raster_stride_bytes(src);
    for (uint32_t y = 0; y < 4; y++) {
      for (uint32_t x = 0; x < 8; x++) {
        memset(p + (size_t)y * stride + (size_t)x * bpp,
            (int)(x * 16u + y), bpp);
      }
    }
    GIMG_Raster * dst = nullptr;
    ASSERT_EQ(gimg_ops_crop(src, 2, 1, 3, 2, &dst), GIMG_OK);
    const unsigned char * dp =
        (const unsigned char *)gimg_raster_pixels_const(dst);
    size_t dstride = gimg_raster_stride_bytes(dst);
    for (uint32_t y = 0; y < 2; y++) {
      for (uint32_t x = 0; x < 3; x++) {
        const unsigned char * px = dp + (size_t)y * dstride + (size_t)x * bpp;
        for (size_t b = 0; b < bpp; b++) {
          EXPECT_EQ(px[b], (unsigned char)((2u + x) * 16u + (1u + y)));
        }
      }
    }
    gimg_raster_destroy(dst);
    gimg_raster_destroy(src);
  }
}

namespace {

GIMG_Raster * make_rgba8(uint32_t w, uint32_t h, unsigned char r,
    unsigned char g, unsigned char b, unsigned char a) {
  GIMG_Raster * ras = nullptr;
  if (gimg_raster_create(w, h, &GIMG_PIXEL_RGBA8, GIMG_RASTER_OWNED, nullptr,
          0, &ras) != GIMG_OK) {
    return nullptr;
  }
  for (uint32_t y = 0; y < h; y++) {
    for (uint32_t x = 0; x < w; x++) {
      unsigned char * p = (unsigned char *)gimg_raster_pixels(ras) +
          (size_t)y * gimg_raster_stride_bytes(ras) + (size_t)x * 4u;
      p[0] = r;
      p[1] = g;
      p[2] = b;
      p[3] = a;
    }
  }
  return ras;
}

const unsigned char * at(const GIMG_Raster * r, uint32_t x, uint32_t y) {
  return (const unsigned char *)gimg_raster_pixels_const(r) +
      (size_t)y * gimg_raster_stride_bytes(r) + (size_t)x * 4u;
}

} // namespace

TEST(Composite, SourceReplacesExactlyTheRectangleItCovers) {
  GIMG_Raster * dst = make_rgba8(10, 8, 10u, 20u, 30u, 255u);
  GIMG_Raster * src = make_rgba8(3, 2, 200u, 100u, 50u, 128u);
  ASSERT_NE(dst, nullptr);
  ASSERT_NE(src, nullptr);
  ASSERT_EQ(gimg_ops_composite(dst, src, 4, 3, GIMG_COMPOSITE_SOURCE),
      GIMG_OK);
  for (uint32_t y = 0; y < 8; y++) {
    for (uint32_t x = 0; x < 10; x++) {
      const unsigned char * p = at(dst, x, y);
      const bool inside = (x >= 4 && x < 7 && y >= 3 && y < 5);
      if (inside) {
        EXPECT_EQ(p[0], 200u) << x << "," << y;
        EXPECT_EQ(p[3], 128u) << x << "," << y;
      }
      else {
        EXPECT_EQ(p[0], 10u) << x << "," << y;
        EXPECT_EQ(p[3], 255u) << x << "," << y;
      }
    }
  }
  gimg_raster_destroy(src);
  gimg_raster_destroy(dst);
}

/**
 * A source hanging off each edge is clipped, and one entirely outside draws
 * nothing and says so with GIMG_OK. Negative offsets are the case a plain
 * unsigned interface cannot express at all.
 */
TEST(Composite, ASourceIsClippedToTheDestination) {
  const int32_t offsets[][2] = {{-2, -2}, {8, 6}, {-2, 3}, {4, -1}, {0, 0},
      {6, 4}};
  for (const auto & off : offsets) {
    GIMG_Raster * dst = make_rgba8(10, 8, 0u, 0u, 0u, 255u);
    // Each source pixel says where in the source it came from. A uniform
    // source cannot tell a correct clip from one that reads the source from
    // its corner regardless of the offset, which is exactly what a negative
    // offset gets wrong.
    GIMG_Raster * src = make_rgba8(4, 4, 0u, 0u, 0u, 255u);
    ASSERT_NE(src, nullptr);
    for (uint32_t sy = 0; sy < 4; sy++) {
      for (uint32_t sx = 0; sx < 4; sx++) {
        unsigned char * p = (unsigned char *)gimg_raster_pixels(src) +
            (size_t)sy * gimg_raster_stride_bytes(src) + (size_t)sx * 4u;
        p[0] = (unsigned char)(0x10u + sx);
        p[1] = (unsigned char)(0x20u + sy);
        p[2] = 0xEEu;
      }
    }
    ASSERT_EQ(gimg_ops_composite(dst, src, off[0], off[1],
                  GIMG_COMPOSITE_SOURCE),
        GIMG_OK);
    for (uint32_t y = 0; y < 8; y++) {
      for (uint32_t x = 0; x < 10; x++) {
        const bool inside = ((int64_t)x >= off[0] && (int64_t)x < off[0] + 4 &&
            (int64_t)y >= off[1] && (int64_t)y < off[1] + 4);
        const unsigned char * p = at(dst, x, y);
        if (inside) {
          const uint32_t sx = (uint32_t)((int64_t)x - off[0]);
          const uint32_t sy = (uint32_t)((int64_t)y - off[1]);
          EXPECT_EQ(p[0], (unsigned char)(0x10u + sx))
              << "offset " << off[0] << "," << off[1] << ": wrong source "
              << "column landed at " << x << "," << y;
          EXPECT_EQ(p[1], (unsigned char)(0x20u + sy))
              << "offset " << off[0] << "," << off[1] << ": wrong source row "
              << "landed at " << x << "," << y;
        }
        else {
          EXPECT_EQ(p[2], 0u)
              << "offset " << off[0] << "," << off[1] << " drew outside at "
              << x << "," << y;
        }
      }
    }
    gimg_raster_destroy(src);
    gimg_raster_destroy(dst);
  }

  // Entirely outside, including offsets far enough out that a 32-bit sum of
  // the offset and the width would wrap back into the destination.
  const int32_t outside[][2] = {{-4, 0}, {10, 0}, {0, -4}, {0, 8},
      {INT32_MAX - 1, 0}, {INT32_MIN, 0}, {0, INT32_MIN}};
  for (const auto & off : outside) {
    GIMG_Raster * dst = make_rgba8(10, 8, 77u, 77u, 77u, 255u);
    GIMG_Raster * src = make_rgba8(4, 4, 1u, 2u, 3u, 255u);
    EXPECT_EQ(gimg_ops_composite(dst, src, off[0], off[1],
                  GIMG_COMPOSITE_SOURCE),
        GIMG_OK)
        << off[0] << "," << off[1];
    for (uint32_t y = 0; y < 8; y++) {
      for (uint32_t x = 0; x < 10; x++) {
        ASSERT_EQ(at(dst, x, y)[0], 77u)
            << "offset " << off[0] << "," << off[1] << " drew at " << x << ","
            << y;
      }
    }
    gimg_raster_destroy(src);
    gimg_raster_destroy(dst);
  }
}

/** A wholly transparent source over anything leaves it untouched. */
TEST(Composite, OverWithNothingVisibleChangesNothing) {
  GIMG_Raster * dst = make_rgba8(6, 6, 33u, 66u, 99u, 200u);
  GIMG_Raster * src = make_rgba8(6, 6, 255u, 0u, 0u, 0u);
  ASSERT_EQ(gimg_ops_composite(dst, src, 0, 0, GIMG_COMPOSITE_OVER), GIMG_OK);
  for (uint32_t y = 0; y < 6; y++) {
    for (uint32_t x = 0; x < 6; x++) {
      const unsigned char * p = at(dst, x, y);
      EXPECT_EQ(p[0], 33u);
      EXPECT_EQ(p[1], 66u);
      EXPECT_EQ(p[2], 99u);
      EXPECT_EQ(p[3], 200u);
    }
  }
  gimg_raster_destroy(src);
  gimg_raster_destroy(dst);
}

/** A wholly opaque source over anything replaces it exactly. */
TEST(Composite, OverWithAnOpaqueSourceIsAReplacement) {
  GIMG_Raster * dst = make_rgba8(6, 6, 33u, 66u, 99u, 40u);
  GIMG_Raster * src = make_rgba8(6, 6, 7u, 8u, 9u, 255u);
  ASSERT_EQ(gimg_ops_composite(dst, src, 0, 0, GIMG_COMPOSITE_OVER), GIMG_OK);
  for (uint32_t y = 0; y < 6; y++) {
    for (uint32_t x = 0; x < 6; x++) {
      const unsigned char * p = at(dst, x, y);
      EXPECT_EQ(p[0], 7u);
      EXPECT_EQ(p[1], 8u);
      EXPECT_EQ(p[2], 9u);
      EXPECT_EQ(p[3], 255u);
    }
  }
  gimg_raster_destroy(src);
  gimg_raster_destroy(dst);
}

/**
 * Half-covering opaque white over opaque black gives the halfway grey, and the
 * result stays opaque.
 *
 * Checked against the arithmetic written out rather than against the
 * implementation: out_a = 255, and out_c = (255*128 + 0*255*127/255) / 255.
 */
TEST(Composite, OverHalfwayIsTheHalfwayColour) {
  GIMG_Raster * dst = make_rgba8(4, 4, 0u, 0u, 0u, 255u);
  GIMG_Raster * src = make_rgba8(4, 4, 255u, 255u, 255u, 128u);
  ASSERT_EQ(gimg_ops_composite(dst, src, 0, 0, GIMG_COMPOSITE_OVER), GIMG_OK);
  for (uint32_t y = 0; y < 4; y++) {
    for (uint32_t x = 0; x < 4; x++) {
      const unsigned char * p = at(dst, x, y);
      EXPECT_EQ(p[3], 255u) << "opaque under anything stays opaque";
      for (uint8_t c = 0; c < 3u; c++) {
        EXPECT_NEAR(p[c], 128, 1) << "channel " << (int)c;
      }
    }
  }
  gimg_raster_destroy(src);
  gimg_raster_destroy(dst);
}

/**
 * Where the result is wholly transparent the colour is zero, whatever was
 * underneath.
 *
 * Otherwise compositing the same source over two different destinations gives
 * different bytes in the pixels where nothing at all is visible, and a
 * byte-comparison of two runs disagrees about pixels nobody can see.
 */
TEST(Composite, AnInvisibleResultCarriesNoColour) {
  GIMG_Raster * a = make_rgba8(4, 4, 200u, 100u, 50u, 0u);
  GIMG_Raster * b = make_rgba8(4, 4, 9u, 9u, 9u, 0u);
  GIMG_Raster * src = make_rgba8(4, 4, 250u, 250u, 250u, 0u);
  ASSERT_EQ(gimg_ops_composite(a, src, 0, 0, GIMG_COMPOSITE_OVER), GIMG_OK);
  ASSERT_EQ(gimg_ops_composite(b, src, 0, 0, GIMG_COMPOSITE_OVER), GIMG_OK);
  EXPECT_TRUE(gimg_ops_raster_equal(a, b))
      << "two transparent destinations composited with the same source "
         "disagree where nothing is visible";
  gimg_raster_destroy(src);
  gimg_raster_destroy(b);
  gimg_raster_destroy(a);
}

/**
 * Half-covering white over half-covering black, where neither the source nor
 * the destination is opaque and the composite alpha is therefore neither.
 *
 * Worked out by hand from the Porter-Duff definition rather than from the
 * code: coverage is 0.5 + 0.5 * 0.5 = 0.75, which is 191.25 and rounds to
 * 192; the colour is (1 * 0.5 + 0 * 0.5 * 0.5) / 0.75 = 2/3, which is 170.
 *
 * The opaque-destination case cannot check the division at all, because there
 * the composite alpha comes to 255 and dividing by it changes nothing - an
 * implementation that skipped the division entirely passes that test and
 * fails this one.
 */
TEST(Composite, OverTwoTranslucentLayersFollowsPorterDuff) {
  GIMG_Raster * dst = make_rgba8(4, 4, 0u, 0u, 0u, 128u);
  GIMG_Raster * src = make_rgba8(4, 4, 255u, 255u, 255u, 128u);
  ASSERT_NE(dst, nullptr);
  ASSERT_NE(src, nullptr);
  ASSERT_EQ(gimg_ops_composite(dst, src, 0, 0, GIMG_COMPOSITE_OVER), GIMG_OK);
  for (uint32_t y = 0; y < 4; y++) {
    for (uint32_t x = 0; x < 4; x++) {
      const unsigned char * p = at(dst, x, y);
      EXPECT_EQ(p[3], 192u) << "coverage at " << x << "," << y;
      for (uint8_t c = 0; c < 3u; c++) {
        EXPECT_NEAR(p[c], 170, 1)
            << "channel " << (int)c << " at " << x << "," << y;
      }
    }
  }
  gimg_raster_destroy(src);
  gimg_raster_destroy(dst);
}

TEST(Composite, WhatIsRefused) {
  GIMG_Raster * rgba = make_rgba8(4, 4, 0u, 0u, 0u, 255u);
  ASSERT_NE(rgba, nullptr);
  EXPECT_EQ(gimg_ops_composite(nullptr, rgba, 0, 0, GIMG_COMPOSITE_SOURCE),
      GIMG_ERR_INTERNAL);
  EXPECT_EQ(gimg_ops_composite(rgba, nullptr, 0, 0, GIMG_COMPOSITE_SOURCE),
      GIMG_ERR_INTERNAL);
  EXPECT_EQ(gimg_ops_composite(rgba, rgba, 0, 0, (GIMG_Composite_Op)42),
      GIMG_ERR_UNSUPPORTED);

  // Mismatched formats: converting unasked would change the picture's colour.
  GIMG_Raster * gray = nullptr;
  ASSERT_EQ(gimg_raster_create(4, 4, &GIMG_PIXEL_GRAY8, GIMG_RASTER_OWNED,
                nullptr, 0, &gray),
      GIMG_OK);
  EXPECT_EQ(gimg_ops_composite(rgba, gray, 0, 0, GIMG_COMPOSITE_SOURCE),
      GIMG_ERR_UNSUPPORTED);

  // "Over" needs an alpha channel to mean anything.
  GIMG_Raster * gray2 = nullptr;
  ASSERT_EQ(gimg_raster_create(4, 4, &GIMG_PIXEL_GRAY8, GIMG_RASTER_OWNED,
                nullptr, 0, &gray2),
      GIMG_OK);
  EXPECT_EQ(gimg_ops_composite(gray, gray2, 0, 0, GIMG_COMPOSITE_OVER),
      GIMG_ERR_UNSUPPORTED);
  // ...but replacing works for a format with no alpha.
  EXPECT_EQ(gimg_ops_composite(gray, gray2, 0, 0, GIMG_COMPOSITE_SOURCE),
      GIMG_OK);

  gimg_raster_destroy(gray2);
  gimg_raster_destroy(gray);
  gimg_raster_destroy(rgba);
}

/**
 * Each named turn is the orientation it claims to be.
 *
 * These forward rather than reimplement, so what is worth checking is that
 * the forwarding is not crossed - a clockwise turn wired to the
 * anticlockwise constant is invisible on a square image of uniform colour and
 * obvious on a rectangle whose pixels say where they are.
 */
TEST(NamedTurns, EachIsTheOrientationItNames) {
  struct Case {
    GIMG_Result (*fn)(GIMG_Raster *);
    GIMG_Orientation equivalent;
    const char * name;
  };
  const Case cases[] = {
      {gimg_ops_flip_horizontal, GIMG_ORIENTATION_FLIP_H, "flip_horizontal"},
      {gimg_ops_flip_vertical, GIMG_ORIENTATION_FLIP_V, "flip_vertical"},
      {gimg_ops_rotate_90_cw, GIMG_ORIENTATION_ROTATE_90_CW, "rotate_90_cw"},
      {gimg_ops_rotate_90_ccw, GIMG_ORIENTATION_ROTATE_90_CCW,
          "rotate_90_ccw"},
      {gimg_ops_rotate_180, GIMG_ORIENTATION_ROTATE_180, "rotate_180"},
  };
  for (const auto & c : cases) {
    // Deliberately not square: four of the six exchange the axes, and a
    // square image hides a turn wired to the wrong constant.
    GIMG_Raster * a = make_positional_rgba8(7, 4);
    GIMG_Raster * b = make_positional_rgba8(7, 4);
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    ASSERT_EQ(c.fn(a), GIMG_OK) << c.name;
    ASSERT_EQ(gimg_ops_apply_orientation(b, c.equivalent), GIMG_OK) << c.name;
    EXPECT_EQ(gimg_raster_width(a), gimg_raster_width(b)) << c.name;
    EXPECT_EQ(gimg_raster_height(a), gimg_raster_height(b)) << c.name;
    EXPECT_TRUE(gimg_ops_raster_equal(a, b)) << c.name;
    gimg_raster_destroy(b);
    gimg_raster_destroy(a);
  }
}

/** Each turn, done four times (or twice), comes back where it started. */
TEST(NamedTurns, TheyComeBackRoundAgain) {
  GIMG_Raster * original = make_positional_rgba8(7, 4);
  ASSERT_NE(original, nullptr);

  GIMG_Raster * r = make_positional_rgba8(7, 4);
  for (int i = 0; i < 4; i++) {
    ASSERT_EQ(gimg_ops_rotate_90_cw(r), GIMG_OK);
  }
  EXPECT_TRUE(gimg_ops_raster_equal(r, original)) << "four quarter turns";
  gimg_raster_destroy(r);

  r = make_positional_rgba8(7, 4);
  ASSERT_EQ(gimg_ops_rotate_90_cw(r), GIMG_OK);
  ASSERT_EQ(gimg_ops_rotate_90_ccw(r), GIMG_OK);
  EXPECT_TRUE(gimg_ops_raster_equal(r, original)) << "one turn each way";
  gimg_raster_destroy(r);

  for (auto fn : {gimg_ops_flip_horizontal, gimg_ops_flip_vertical,
           gimg_ops_rotate_180}) {
    r = make_positional_rgba8(7, 4);
    ASSERT_EQ(fn(r), GIMG_OK);
    ASSERT_EQ(fn(r), GIMG_OK);
    EXPECT_TRUE(gimg_ops_raster_equal(r, original)) << "twice is the identity";
    gimg_raster_destroy(r);
  }
  gimg_raster_destroy(original);
}

int main(int argc, char ** argv) {
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}

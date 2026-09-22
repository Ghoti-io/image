/**
 * @file
 *
 * Unit tests for the geometry operations: cropping.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <ghoti.io/image/color.h>
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

int main(int argc, char ** argv) {
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}

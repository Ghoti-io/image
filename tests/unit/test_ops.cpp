/**
 * @file
 *
 * Unit tests for orientation, pixel format conversion, alpha.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <ghoti.io/image/ops.h>
#include <ghoti.io/image/raster.h>
#include <gtest/gtest.h>

TEST(Ops, ApplyOrientationIdentity) {
  GIMG_Raster * r = nullptr;
  gimg_raster_create(
      2, 2, &GIMG_PIXEL_RGBA8, GIMG_RASTER_OWNED, nullptr, 0, &r);
  ASSERT_NE(r, nullptr);
  GIMG_Result res = gimg_ops_apply_orientation(r, GIMG_ORIENTATION_NORMAL);
  EXPECT_EQ(res, GIMG_OK);
  gimg_raster_destroy(r);
}

TEST(Ops, ApplyOrientation180) {
  GIMG_Raster * r = nullptr;
  gimg_raster_create(
      2, 2, &GIMG_PIXEL_RGBA8, GIMG_RASTER_OWNED, nullptr, 0, &r);
  ASSERT_NE(r, nullptr);
  unsigned char * p = (unsigned char *)gimg_raster_pixels(r);
  p[0] = 1;
  p[1] = 2;
  p[2] = 3;
  p[3] = 255;
  GIMG_Result res = gimg_ops_apply_orientation(r, GIMG_ORIENTATION_ROTATE_180);
  ASSERT_EQ(res, GIMG_OK);
  size_t stride = gimg_raster_stride_bytes(r);
  unsigned char * last_row = p + stride * (gimg_raster_height(r) - 1);
  size_t bpp = 4;
  unsigned char * last_px = last_row + (gimg_raster_width(r) - 1) * bpp;
  EXPECT_EQ(last_px[0], 1);
  EXPECT_EQ(last_px[1], 2);
  EXPECT_EQ(last_px[2], 3);
  gimg_raster_destroy(r);
}

TEST(Ops, ConvertSameFormatCopy) {
  GIMG_Raster * src = nullptr;
  gimg_raster_create(
      3, 2, &GIMG_PIXEL_RGBA8, GIMG_RASTER_OWNED, nullptr, 0, &src);
  ASSERT_NE(src, nullptr);
  GIMG_Raster * dst = nullptr;
  GIMG_Result r = gimg_ops_convert_pixel_format(src, &GIMG_PIXEL_RGBA8, &dst);
  ASSERT_EQ(r, GIMG_OK);
  ASSERT_NE(dst, nullptr);
  EXPECT_EQ(gimg_raster_width(dst), 3u);
  EXPECT_EQ(gimg_raster_height(dst), 2u);
  gimg_raster_destroy(src);
  gimg_raster_destroy(dst);
}

TEST(Ops, ConvertUnsupportedReturnsError) {
  GIMG_Raster * src = nullptr;
  gimg_raster_create(
      2, 2, &GIMG_PIXEL_RGBA8, GIMG_RASTER_OWNED, nullptr, 0, &src);
  ASSERT_NE(src, nullptr);
  GIMG_Raster * dst = nullptr;
  GIMG_Result r = gimg_ops_convert_pixel_format(src, &GIMG_PIXEL_GRAY8, &dst);
  EXPECT_EQ(r, GIMG_ERR_UNSUPPORTED);
  EXPECT_EQ(dst, nullptr);
  gimg_raster_destroy(src);
}

TEST(Ops, AlphaPremultiplyUnpremultiplyRoundTrip) {
  GIMG_Raster * r = nullptr;
  gimg_raster_create(
      1, 1, &GIMG_PIXEL_RGBA8, GIMG_RASTER_OWNED, nullptr, 0, &r);
  ASSERT_NE(r, nullptr);
  unsigned char * p = (unsigned char *)gimg_raster_pixels(r);
  p[0] = 100;
  p[1] = 150;
  p[2] = 200;
  p[3] = 128;
  GIMG_Result r1 = gimg_alpha_premultiply(r);
  ASSERT_EQ(r1, GIMG_OK);
  GIMG_Result r2 = gimg_alpha_unpremultiply(r);
  ASSERT_EQ(r2, GIMG_OK);
  EXPECT_NEAR((int)p[0], 100, 2);
  EXPECT_NEAR((int)p[1], 150, 2);
  EXPECT_NEAR((int)p[2], 200, 2);
  EXPECT_EQ(p[3], 128);
  gimg_raster_destroy(r);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}

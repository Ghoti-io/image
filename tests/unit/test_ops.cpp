/**
 * @file
 *
 * Unit tests for orientation, pixel format conversion, alpha.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <ghoti.io/image/bitdepth.h>
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

TEST(Ops, BitdepthSampleConversions) {
  EXPECT_EQ(gimg_bitdepth_8_to_12(0), 0);
  EXPECT_EQ(gimg_bitdepth_8_to_12(255), 4095u);
  EXPECT_EQ(gimg_bitdepth_8_to_16(0), 0);
  EXPECT_EQ(gimg_bitdepth_8_to_16(255), 65535u);
  EXPECT_EQ(gimg_bitdepth_12_to_8(0), 0);
  EXPECT_EQ(gimg_bitdepth_12_to_8(4095), 255);
  EXPECT_EQ(gimg_bitdepth_12_to_16(4095), 65520u);
  EXPECT_EQ(gimg_bitdepth_16_to_8(0), 0);
  EXPECT_EQ(gimg_bitdepth_16_to_8(65535), 255);
  EXPECT_EQ(gimg_bitdepth_16_to_12(65535), 4095u);
}

TEST(Ops, ConvertBitDepthGray8To16) {
  GIMG_Raster * src = nullptr;
  gimg_raster_create(
      2, 2, &GIMG_PIXEL_GRAY8, GIMG_RASTER_OWNED, nullptr, 0, &src);
  ASSERT_NE(src, nullptr);
  size_t src_stride = gimg_raster_stride_bytes(src);
  unsigned char * p = (unsigned char *)gimg_raster_pixels(src);
  p[0] = 0;
  p[1] = 255;
  p[0 + src_stride] = 128;
  p[1 + src_stride] = 64;
  GIMG_Raster * dst = nullptr;
  GIMG_Result r = gimg_ops_convert_bit_depth(src, 16, &dst);
  ASSERT_EQ(r, GIMG_OK);
  ASSERT_NE(dst, nullptr);
  EXPECT_EQ(gimg_raster_width(dst), 2u);
  EXPECT_EQ(gimg_raster_height(dst), 2u);
  EXPECT_EQ(gimg_raster_format(dst)->bits_per_channel[0], 16);
  size_t stride = gimg_raster_stride_bytes(dst);
  const unsigned char * base =
      (const unsigned char *)gimg_raster_pixels_const(dst);
  const uint16_t * row0 = (const uint16_t *)base;
  const uint16_t * row1 = (const uint16_t *)(base + stride);
  EXPECT_EQ(row0[0], 0);
  EXPECT_EQ(row0[1], 65535u);
  EXPECT_EQ(row1[0], (128u << 8) | 128u);  // 8→16 replicate (v<<8)|v
  EXPECT_EQ(row1[1], (64u << 8) | 64u);
  gimg_raster_destroy(src);
  gimg_raster_destroy(dst);
}

TEST(Ops, ConvertBitDepthGray16To12) {
  GIMG_Raster * src = nullptr;
  gimg_raster_create(
      1, 2, &GIMG_PIXEL_GRAY16, GIMG_RASTER_OWNED, nullptr, 0, &src);
  ASSERT_NE(src, nullptr);
  size_t src_stride = gimg_raster_stride_bytes(src);
  unsigned char * base = (unsigned char *)gimg_raster_pixels(src);
  *(uint16_t *)base = 0;
  *(uint16_t *)(base + src_stride) = 65535;
  GIMG_Raster * dst = nullptr;
  GIMG_Result r = gimg_ops_convert_bit_depth(src, 12, &dst);
  ASSERT_EQ(r, GIMG_OK);
  ASSERT_NE(dst, nullptr);
  EXPECT_EQ(gimg_raster_format(dst)->bits_per_channel[0], 12);
  size_t dst_stride = gimg_raster_stride_bytes(dst);
  const unsigned char * dbase =
      (const unsigned char *)gimg_raster_pixels_const(dst);
  EXPECT_EQ(*(const uint16_t *)dbase, 0);
  EXPECT_EQ(*(const uint16_t *)(dbase + dst_stride), 4095u);
  gimg_raster_destroy(src);
  gimg_raster_destroy(dst);
}

TEST(Ops, ConvertBitDepthInvalidDstBitsReturnsError) {
  GIMG_Raster * src = nullptr;
  gimg_raster_create(
      1, 1, &GIMG_PIXEL_GRAY8, GIMG_RASTER_OWNED, nullptr, 0, &src);
  ASSERT_NE(src, nullptr);
  GIMG_Raster * dst = nullptr;
  GIMG_Result r = gimg_ops_convert_bit_depth(src, 7, &dst);
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

TEST(Ops, RasterEqualIdentical) {
  GIMG_Raster * a = nullptr;
  GIMG_Raster * b = nullptr;
  gimg_raster_create(
      2, 2, &GIMG_PIXEL_RGBA8, GIMG_RASTER_OWNED, nullptr, 0, &a);
  gimg_raster_create(
      2, 2, &GIMG_PIXEL_RGBA8, GIMG_RASTER_OWNED, nullptr, 0, &b);
  ASSERT_NE(a, nullptr);
  ASSERT_NE(b, nullptr);
  unsigned char * pa = (unsigned char *)gimg_raster_pixels(a);
  unsigned char * pb = (unsigned char *)gimg_raster_pixels(b);
  for (int i = 0; i < 16; i++) {
    pa[i] = pb[i] = (unsigned char)i;
  }
  EXPECT_TRUE(gimg_ops_raster_equal(a, b));
  gimg_raster_destroy(a);
  gimg_raster_destroy(b);
}

TEST(Ops, RasterEqualDifferentDimensions) {
  GIMG_Raster * a = nullptr;
  GIMG_Raster * b = nullptr;
  gimg_raster_create(
      2, 2, &GIMG_PIXEL_RGBA8, GIMG_RASTER_OWNED, nullptr, 0, &a);
  gimg_raster_create(
      2, 3, &GIMG_PIXEL_RGBA8, GIMG_RASTER_OWNED, nullptr, 0, &b);
  ASSERT_NE(a, nullptr);
  ASSERT_NE(b, nullptr);
  EXPECT_FALSE(gimg_ops_raster_equal(a, b));
  gimg_raster_destroy(a);
  gimg_raster_destroy(b);
}

TEST(Ops, RasterEqualOnePixelDifferent) {
  GIMG_Raster * a = nullptr;
  GIMG_Raster * b = nullptr;
  gimg_raster_create(
      2, 2, &GIMG_PIXEL_RGBA8, GIMG_RASTER_OWNED, nullptr, 0, &a);
  gimg_raster_create(
      2, 2, &GIMG_PIXEL_RGBA8, GIMG_RASTER_OWNED, nullptr, 0, &b);
  ASSERT_NE(a, nullptr);
  ASSERT_NE(b, nullptr);
  unsigned char * pa = (unsigned char *)gimg_raster_pixels(a);
  unsigned char * pb = (unsigned char *)gimg_raster_pixels(b);
  for (int i = 0; i < 16; i++) {
    pa[i] = pb[i] = (unsigned char)i;
  }
  pb[5] = 99;
  EXPECT_FALSE(gimg_ops_raster_equal(a, b));
  gimg_raster_destroy(a);
  gimg_raster_destroy(b);
}

TEST(Ops, RasterEqualNull) {
  GIMG_Raster * a = nullptr;
  gimg_raster_create(
      1, 1, &GIMG_PIXEL_RGBA8, GIMG_RASTER_OWNED, nullptr, 0, &a);
  ASSERT_NE(a, nullptr);
  EXPECT_FALSE(gimg_ops_raster_equal(nullptr, a));
  EXPECT_FALSE(gimg_ops_raster_equal(a, nullptr));
  gimg_raster_destroy(a);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}

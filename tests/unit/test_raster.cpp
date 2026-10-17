/**
 * @file
 *
 * Unit tests for GIMG_Raster create/destroy and accessors.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <ghoti.io/image/raster.h>
#include <gtest/gtest.h>
#include <vector>

TEST(Raster, CreateOwned) {
  GIMG_Raster * r = nullptr;
  GIMG_Result res = gimg_raster_create(
      10, 20, &GIMG_PIXEL_RGBA8, GIMG_RASTER_OWNED, nullptr, 0, &r);
  ASSERT_EQ(res, GIMG_OK);
  ASSERT_NE(r, nullptr);
  EXPECT_EQ(gimg_raster_width(r), 10u);
  EXPECT_EQ(gimg_raster_height(r), 20u);
  EXPECT_GE(gimg_raster_stride_bytes(r), 40u);
  EXPECT_EQ(gimg_raster_ownership(r), GIMG_RASTER_OWNED);
  EXPECT_NE(gimg_raster_pixels(r), nullptr);
  EXPECT_EQ(gimg_raster_format(r)->channel_model, GIMG_CHANNEL_RGBA);
  gimg_raster_destroy(r);
}

TEST(Raster, BorrowedViewDoesNotFree) {
  unsigned char buf[40 * 5] = {0};
  GIMG_Raster * r = nullptr;
  GIMG_Result res = gimg_raster_create(
      10, 5, &GIMG_PIXEL_RGBA8, GIMG_RASTER_BORROWED, buf, 40, &r);
  ASSERT_EQ(res, GIMG_OK);
  ASSERT_NE(r, nullptr);
  EXPECT_EQ(gimg_raster_pixels(r), buf);
  EXPECT_EQ(gimg_raster_ownership(r), GIMG_RASTER_BORROWED);
  gimg_raster_destroy(r); // Must not free buf
  (void)buf[0];           // Use after destroy to ensure buf still valid
}

TEST(Raster, DestroyNullNoOp) {
  gimg_raster_destroy(nullptr);
}

TEST(Raster, BytesPerPixel) {
  EXPECT_EQ(gimg_raster_bytes_per_pixel(&GIMG_PIXEL_RGBA8), 4u);
  EXPECT_EQ(gimg_raster_bytes_per_pixel(&GIMG_PIXEL_GRAY8), 1u);
}

TEST(Raster, CopyDimensionsAndFormat) {
  GIMG_Raster * src = nullptr;
  ASSERT_EQ(gimg_raster_create(
                8, 4, &GIMG_PIXEL_RGBA8, GIMG_RASTER_OWNED, nullptr, 0, &src),
      GIMG_OK);
  ASSERT_NE(src, nullptr);
  unsigned char * p = (unsigned char *)gimg_raster_pixels(src);
  for (size_t i = 0; i < 8u * 4u * 4; i++) {
    p[i] = (unsigned char)(i & 0xFF);
  }
  GIMG_Raster * copy = nullptr;
  GIMG_Result res = gimg_raster_copy(src, &copy);
  ASSERT_EQ(res, GIMG_OK);
  ASSERT_NE(copy, nullptr);
  EXPECT_EQ(gimg_raster_width(copy), 8u);
  EXPECT_EQ(gimg_raster_height(copy), 4u);
  EXPECT_EQ(gimg_raster_format(copy)->channel_model, GIMG_CHANNEL_RGBA);
  size_t bpp = 4u;
  size_t row_bytes = 8 * bpp;
  const unsigned char * cp = (const unsigned char *)gimg_raster_pixels_const(copy);
  size_t stride = gimg_raster_stride_bytes(copy);
  for (uint32_t y = 0; y < 4; y++) {
    for (size_t i = 0; i < row_bytes; i++) {
      EXPECT_EQ(cp[y * stride + i], p[y * gimg_raster_stride_bytes(src) + i])
          << "y=" << y << " i=" << i;
    }
  }
  gimg_raster_destroy(copy);
  gimg_raster_destroy(src);
}

TEST(Raster, CopySourceWithStride) {
  size_t src_stride = 64;
  size_t row_bytes = 8 * 4;
  std::vector<unsigned char> buf(src_stride * 4, 0);
  for (uint32_t y = 0; y < 4; y++) {
    for (size_t x = 0; x < row_bytes; x++) {
      buf[y * src_stride + x] = (unsigned char)(y * row_bytes + x + 1);
    }
  }
  GIMG_Raster * src = nullptr;
  ASSERT_EQ(gimg_raster_create(8, 4, &GIMG_PIXEL_RGBA8, GIMG_RASTER_BORROWED,
                buf.data(), src_stride, &src),
      GIMG_OK);
  GIMG_Raster * copy = nullptr;
  ASSERT_EQ(gimg_raster_copy(src, &copy), GIMG_OK);
  ASSERT_NE(copy, nullptr);
  EXPECT_EQ(gimg_raster_width(copy), 8u);
  EXPECT_EQ(gimg_raster_height(copy), 4u);
  EXPECT_EQ(gimg_raster_stride_bytes(copy), 32u);
  const unsigned char * cp =
      (const unsigned char *)gimg_raster_pixels_const(copy);
  for (uint32_t y = 0; y < 4; y++) {
    for (size_t x = 0; x < row_bytes; x++) {
      EXPECT_EQ(cp[y * 32 + x], (unsigned char)(y * row_bytes + x + 1));
    }
  }
  gimg_raster_destroy(copy);
  gimg_raster_destroy(src);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}

// ---- Alpha form (AD-26) ----------------------------------------------------

TEST(RasterAlphaForm, BuiltInConstants) {
  EXPECT_EQ(GIMG_PIXEL_RGBA8.alpha_form, GIMG_ALPHA_STRAIGHT);
  EXPECT_EQ(GIMG_PIXEL_RGBA16.alpha_form, GIMG_ALPHA_STRAIGHT);
  EXPECT_EQ(GIMG_PIXEL_RGBA12.alpha_form, GIMG_ALPHA_STRAIGHT);
  EXPECT_EQ(GIMG_PIXEL_GRAY8.alpha_form, GIMG_ALPHA_NONE);
  EXPECT_EQ(GIMG_PIXEL_GRAY16.alpha_form, GIMG_ALPHA_NONE);
  EXPECT_EQ(GIMG_PIXEL_GRAY12.alpha_form, GIMG_ALPHA_NONE);
  EXPECT_EQ(GIMG_PIXEL_CMYK8.alpha_form, GIMG_ALPHA_NONE);
  EXPECT_EQ(GIMG_PIXEL_CMYK12.alpha_form, GIMG_ALPHA_NONE);
  EXPECT_EQ(GIMG_PIXEL_CMYK16.alpha_form, GIMG_ALPHA_NONE);
}

TEST(RasterAlphaForm, LegacyZeroBecomesStraightInTheRastersCopy) {
  GIMG_Pixel_Format legacy = GIMG_PIXEL_RGBA8;
  legacy.alpha_form = 0; // a struct built before the field existed
  GIMG_Raster * r = nullptr;
  ASSERT_EQ(gimg_raster_create(
                2, 2, &legacy, GIMG_RASTER_OWNED, nullptr, 0, &r),
      GIMG_OK);
  EXPECT_EQ(gimg_raster_format(r)->alpha_form, GIMG_ALPHA_STRAIGHT);
  EXPECT_EQ(legacy.alpha_form, GIMG_ALPHA_NONE); // caller's struct untouched
  gimg_raster_destroy(r);
}

TEST(RasterAlphaForm, PremultipliedIsKeptAndCopied) {
  GIMG_Pixel_Format f = GIMG_PIXEL_RGBA8;
  f.alpha_form = GIMG_ALPHA_PREMULTIPLIED;
  GIMG_Raster * r = nullptr;
  ASSERT_EQ(gimg_raster_create(
                2, 2, &f, GIMG_RASTER_OWNED, nullptr, 0, &r),
      GIMG_OK);
  EXPECT_EQ(gimg_raster_format(r)->alpha_form, GIMG_ALPHA_PREMULTIPLIED);
  GIMG_Raster * c = nullptr;
  ASSERT_EQ(gimg_raster_copy(r, &c), GIMG_OK);
  EXPECT_EQ(gimg_raster_format(c)->alpha_form, GIMG_ALPHA_PREMULTIPLIED);
  gimg_raster_destroy(c);
  gimg_raster_destroy(r);
}

TEST(RasterAlphaForm, AlphaOnAModelWithoutAlphaIsInvalid) {
  GIMG_Pixel_Format f = GIMG_PIXEL_GRAY8;
  GIMG_Raster * r = nullptr;
  f.alpha_form = GIMG_ALPHA_STRAIGHT;
  EXPECT_EQ(gimg_raster_create(
                2, 2, &f, GIMG_RASTER_OWNED, nullptr, 0, &r),
      GIMG_ERR_INVALID);
  EXPECT_EQ(r, nullptr);
  f.alpha_form = GIMG_ALPHA_PREMULTIPLIED;
  EXPECT_EQ(gimg_raster_create(
                2, 2, &f, GIMG_RASTER_OWNED, nullptr, 0, &r),
      GIMG_ERR_INVALID);
  EXPECT_EQ(r, nullptr);
  // An RGB model (no alpha channel) is the case the matrix names.
  GIMG_Pixel_Format rgb = GIMG_PIXEL_RGBA8;
  rgb.channel_model = GIMG_CHANNEL_RGB;
  rgb.channel_count = 3;
  rgb.bits_per_channel[3] = 0;
  rgb.alpha_form = GIMG_ALPHA_STRAIGHT;
  EXPECT_EQ(gimg_raster_create(
                2, 2, &rgb, GIMG_RASTER_OWNED, nullptr, 0, &r),
      GIMG_ERR_INVALID);
  EXPECT_EQ(r, nullptr);
  rgb.alpha_form = GIMG_ALPHA_NONE;
  EXPECT_EQ(gimg_raster_create(
                2, 2, &rgb, GIMG_RASTER_OWNED, nullptr, 0, &r),
      GIMG_OK);
  EXPECT_EQ(gimg_raster_format(r)->alpha_form, GIMG_ALPHA_NONE);
  gimg_raster_destroy(r);
}

TEST(RasterAlphaForm, ValueOutsideTheEnumIsInvalid) {
  GIMG_Pixel_Format f = GIMG_PIXEL_RGBA8;
  f.alpha_form = 9;
  GIMG_Raster * r = nullptr;
  EXPECT_EQ(gimg_raster_create(
                2, 2, &f, GIMG_RASTER_OWNED, nullptr, 0, &r),
      GIMG_ERR_INVALID);
  EXPECT_EQ(r, nullptr);
}

TEST(RasterAlphaForm, ResultStringTableIsComplete) {
  for (int i = 0; i < GIMG_RESULT_COUNT; i++) {
    const char * s = gimg_result_string((GIMG_Result)i);
    ASSERT_NE(s, nullptr);
    EXPECT_STRNE(s, "unknown") << i;
  }
  EXPECT_STREQ(gimg_result_string(GIMG_ERR_INVALID), "invalid argument");
  EXPECT_STREQ(gimg_result_string(GIMG_RESULT_COUNT), "unknown");
}

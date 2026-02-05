/**
 * @file
 *
 * Unit tests for GIMG_RASTER create/destroy and accessors.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <ghoti.io/image/raster.h>
#include <gtest/gtest.h>

TEST(Raster, CreateOwned) {
  GIMG_RASTER * r = nullptr;
  GIMG_RESULT res = gimg_raster_create(
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
  GIMG_RASTER * r = nullptr;
  GIMG_RESULT res = gimg_raster_create(
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

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
